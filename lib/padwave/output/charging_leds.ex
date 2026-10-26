defmodule Padwave.Output.ChargingLeds do
  @moduledoc """
  Sweeps the five player LEDs while the controller charges, and leaves the
  middle one lit the rest of the time.

  Sony's firmware animates the player LEDs when the pad is on USB; over
  Bluetooth nothing does, so this is ours. The pattern is a slow alternation
  between the two checkerboards — 01010 and 10101 — and when the cable comes
  out the LEDs come back to the middle one (xx3xx), which is also the kernel's
  "player 1" pattern, so handing them to anyone else looks unchanged.
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config}
  alias Padwave.Output.Hid

  # Slow alternation: LEDs 2+4, then 1+3+5, and back. Not a chase — a breath.
  @tick_ms 900
  @pattern [0b01010, 0b10101]
  @home 0b00100

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc false
  def pattern, do: @pattern
  @doc false
  def home, do: @home

  @impl true
  def init(_opts) do
    Bus.subscribe(:sensors)
    Bus.subscribe()
    {:ok, %{charging: false, index: 0, timer: nil}}
  end

  @impl true
  def handle_info({:padwave, {:frame, frame}}, state) do
    # :full means "finished charging but still on power" — animate either way.
    on_power? = frame.charging in [:charging, :full]
    charging? = on_power? and Config.get(:charging_leds, true)

    cond do
      charging? and not state.charging -> {:noreply, start_sweep(state)}
      not charging? and state.charging -> {:noreply, stop_sweep(state)}
      true -> {:noreply, state}
    end
  end

  def handle_info({:padwave, {:device, :down, _info}}, state), do: {:noreply, stop_sweep(state)}

  def handle_info(:tick, state) do
    Hid.player_leds(Enum.at(@pattern, state.index))
    step = Config.get(:charging_leds_ms, @tick_ms)
    timer = Process.send_after(self(), :tick, step)
    {:noreply, %{state | index: rem(state.index + 1, length(@pattern)), timer: timer}}
  end

  def handle_info(_other, state), do: {:noreply, state}

  defp start_sweep(state) do
    if state.timer, do: Process.cancel_timer(state.timer)
    send(self(), :tick)
    %{state | charging: true, index: 0, timer: nil}
  end

  defp stop_sweep(state) do
    if state.timer, do: Process.cancel_timer(state.timer)
    if state.charging, do: Hid.player_leds(@home)
    %{state | charging: false, index: 0, timer: nil}
  end
end
