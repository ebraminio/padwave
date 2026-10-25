defmodule Padwave.Output.Lightbar do
  @moduledoc """
  Drives the DualSense lightbar through the kernel's LED class rather than raw
  HID writes.

  LinuxAudio4Dualsense5 already owns the controller's HID output endpoint for
  audio, so writing our own `0x31` reports could fight with it. The kernel's
  `playstation` driver exposes the very same lightbar as
  `/sys/class/leds/*:rgb:indicator`, which is both safer and instant.

  The colour tells you the state from across the room: cyan while playing,
  amber while paused, a red pulse during the idle warning, dark once the
  controller is off.
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config}

  @pulse_ms 600

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc "Overrides the lightbar colour until the next state change."
  def set_rgb(r, g, b), do: GenServer.cast(__MODULE__, {:rgb, {r, g, b}})

  @impl true
  def init(_opts) do
    Bus.subscribe()

    dir = find_led()

    state = %{
      dir: dir,
      writable: dir != nil and writable?(dir),
      mode: :off,
      bright: false,
      custom: nil
    }

    if state.writable do
      Logger.info("[lightbar] using #{dir}")
      {:ok, paint(state, {0, 0, 0}, 0)}
    else
      if dir, do: Logger.info("[lightbar] #{dir} is not writable; lightbar disabled")
      {:ok, state}
    end
  end

  @impl true
  def handle_cast({:rgb, rgb}, state), do: {:noreply, paint(state, rgb, 200)}

  @impl true
  def handle_info({:padwave, {:player, {:playing, _path}}}, state) do
    {:noreply, light(%{state | mode: :playing, custom: nil})}
  end

  def handle_info({:padwave, {:player, :stopped}}, state) do
    {:noreply, light(%{state | mode: :paused, custom: nil})}
  end

  def handle_info({:padwave, {:session, :warning}}, state) do
    Process.send_after(self(), :pulse, @pulse_ms)
    {:noreply, light(%{state | mode: :warning, custom: nil})}
  end

  def handle_info({:padwave, {:session, :active}}, state) do
    {:noreply, light(%{state | mode: :playing, custom: nil})}
  end

  def handle_info({:padwave, {:radio, {:suspended, _reason}}}, state) do
    {:noreply, light(%{state | mode: :off, custom: nil})}
  end

  def handle_info({:padwave, {:device, :down, _info}}, state) do
    {:noreply, %{state | mode: :off}}
  end

  def handle_info({:padwave, {:device, :up, _info}}, state) do
    # Nothing is necessarily playing yet; the player announces itself.
    {:noreply, light(%{state | mode: :idle, custom: nil})}
  end

  def handle_info(:pulse, %{mode: :warning} = state) do
    Process.send_after(self(), :pulse, @pulse_ms)
    bright = if state.bright, do: 30, else: 255
    {:noreply, paint(%{state | bright: bright}, {255, 0, 0}, bright)}
  end

  def handle_info(:pulse, state), do: {:noreply, state}
  def handle_info(_other, state), do: {:noreply, state}

  # -- painting --------------------------------------------------------------

  defp light(%{writable: false} = state), do: state

  defp light(state), do: paint(state, color(state.mode), brightness(state.mode))

  defp color(:playing), do: {0, 140, 255}
  defp color(:paused), do: {255, 110, 0}
  defp color(:warning), do: {255, 0, 0}
  defp color(_), do: {0, 0, 0}

  defp brightness(:playing), do: 190
  defp brightness(:paused), do: 70
  defp brightness(:warning), do: 255
  defp brightness(_), do: 0

  defp paint(%{writable: false} = state, _rgb, _brightness), do: state

  defp paint(%{dir: dir} = state, {r, g, b}, brightness) do
    _ = File.write(Path.join(dir, "multi_intensity"), "#{r} #{g} #{b}")
    _ = File.write(Path.join(dir, "brightness"), Integer.to_string(brightness))
    state
  end

  defp find_led do
    cond do
      not Config.get(:lightbar_enabled, true) ->
        nil

      true ->
        "/sys/class/leds/*:rgb:indicator"
        |> Path.wildcard()
        |> Enum.find(fn dir ->
          case File.read_link(dir) do
            {:ok, target} -> String.contains?(target, "054C:0CE6")
            _ -> false
          end
        end)
    end
  end

  defp writable?(dir) do
    case File.write(Path.join(dir, "brightness"), "0") do
      :ok -> true
      _ -> false
    end
  end
end
