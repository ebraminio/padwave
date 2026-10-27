defmodule Padwave.Output.Lightbar do
  @moduledoc """
  Drives the DualSense lightbar through our own output reports.

  This used to paint `/sys/class/leds/*:rgb:indicator`. It now goes through
  `Padwave.Output.Hid` for the same reason the trigger effects do: one writer
  for report `0x31`, one place that owns the Bluetooth sequence tag and CRC,
  and no dependency on the kernel's LED class (or its permissions).

  The colour tells you the state from across the room: cyan while playing,
  amber paused, purple muted, a red pulse during the sleep-timer fade, dark once
  the controller is off.
  """

  use GenServer
  alias Padwave.{Bus, Config}
  alias Padwave.Output.Hid

  @pulse_ms 600

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc "Overrides the colour until the next state change."
  def set_rgb(r, g, b), do: GenServer.cast(__MODULE__, {:rgb, {r, g, b}})

  @doc "Drops the override and repaints the state colour."
  def clear, do: GenServer.cast(__MODULE__, :clear)

  @doc "Sets the lightbar brightness field (valid_flag2 bit 0) and repaints."
  def set_brightness(value), do: GenServer.cast(__MODULE__, {:brightness, value})

  @doc "Sends a `lightbar_setup` value (valid_flag2 bit 1) and forgets the paint."
  def set_setup(value), do: GenServer.cast(__MODULE__, {:setup, value})

  @doc "The kernel's fade-out: lightbar_setup bit 1, light out."
  def fade_out, do: GenServer.cast(__MODULE__, {:setup, 0x02})

  @doc "What the bar is showing, for the debug endpoint."
  def status, do: GenServer.call(__MODULE__, :status, 2_000)

  @impl true
  def handle_call(:status, _from, state) do
    {:reply,
     %{
       mode: state.mode,
       muted: state.muted,
       painted: rgb(state.painted),
       custom: rgb(state.custom)
     }, state}
  end

  # Tuples are not JSON, and the debug endpoint is JSON.
  defp rgb(nil), do: nil
  defp rgb({r, g, b}), do: [r, g, b]

  @impl true
  def init(_opts) do
    Bus.subscribe()

    {:ok,
     %{
       mode: :off,
       muted: false,
       bright: false,
       custom: nil,
       painted: nil,
       brightness: nil,
       setup: nil
     }}
  end

  @impl true
  def handle_cast({:rgb, rgb}, state), do: {:noreply, paint(%{state | custom: rgb})}
  def handle_cast(:clear, state), do: {:noreply, paint(%{state | custom: nil})}

  def handle_cast({:brightness, value}, state) do
    state = %{state | brightness: value, painted: nil}
    {:noreply, paint(state)}
  end

  def handle_cast({:setup, value}, state) do
    fields = %{valid_flag2: ["color_fade"], lightbar_setup: value}

    if Config.get(:lightbar_enabled, true), do: Hid.send_fields(fields)
    {:noreply, %{state | setup: value, painted: nil}}
  end

  @impl true
  def handle_info({:padwave, {:player, {:playing, _path}}}, state), do: {:noreply, set(state, :playing)}
  def handle_info({:padwave, {:player, :stopped}}, state), do: {:noreply, set(state, :paused)}

  def handle_info({:padwave, {:session, :warning}}, state) do
    Process.send_after(self(), :pulse, @pulse_ms)
    {:noreply, set(state, :warning)}
  end

  def handle_info({:padwave, {:session, :active}}, state), do: {:noreply, set(state, :playing)}
  def handle_info({:padwave, {:radio, {:suspended, _reason}}}, state), do: {:noreply, set(state, :off)}

  # Transport and mute are independent: unmuting returns to whatever the
  # transport was, not to "playing".
  def handle_info({:padwave, {:radio, {:transport, transport}}}, state)
      when transport in [:playing, :paused],
      do: {:noreply, set(state, transport)}

  def handle_info({:padwave, {:radio, {:volume, 0}}}, state) do
    {:noreply, paint(%{state | muted: true, custom: nil})}
  end

  def handle_info({:padwave, {:radio, {:volume, _level}}}, state) do
    {:noreply, paint(%{state | muted: false})}
  end

  def handle_info({:padwave, {:device, :down, _info}}, state), do: {:noreply, %{state | mode: :off}}

  def handle_info({:padwave, {:device, :up, _info}}, state) do
    # The controller comes back with the kernel's startup animation. Keep the
    # user's custom colour and force one repaint, even if the target colour is
    # the one we last painted before the disconnect.
    {:noreply, paint(%{state | painted: nil})}
  end

  def handle_info(:pulse, %{mode: :warning} = state) do
    Process.send_after(self(), :pulse, @pulse_ms)
    bright = if state.bright, do: 60, else: 255
    Hid.lightbar(scale({255, 0, 0}, bright))
    {:noreply, %{state | bright: bright}}
  end

  def handle_info(:pulse, state), do: {:noreply, state}
  def handle_info(_other, state), do: {:noreply, state}

  # -- painting --------------------------------------------------------------

  defp set(state, mode), do: paint(%{state | mode: mode, custom: nil})

  defp paint(%{custom: nil, muted: true} = state), do: do_paint(state, color(:muted))
  defp paint(%{custom: nil} = state), do: do_paint(state, color(state.mode))
  defp paint(%{custom: rgb} = state), do: do_paint(state, rgb)

  # Only write when the colour actually changes: the volume is broadcast on
  # every trigger step, and repainting an unchanged bar would waste HID reports.
  defp do_paint(%{painted: rgb} = state, rgb), do: state

  defp do_paint(state, {r, g, b}) do
    if Config.get(:lightbar_enabled, true) do
      Hid.lightbar(r, g, b, brightness: state.brightness)
    end

    %{state | painted: {r, g, b}}
  end

  defp scale({r, g, b}, 255), do: {r, g, b}
  defp scale({r, g, b}, k), do: {round(r * k / 255), round(g * k / 255), round(b * k / 255)}

  defp color(:playing), do: {0, 140, 255}
  defp color(:paused), do: {255, 110, 0}
  defp color(:muted), do: {120, 60, 255}
  defp color(:warning), do: {255, 0, 0}
  defp color(_), do: {0, 0, 0}
end
