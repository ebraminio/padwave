defmodule Padwave.Output.ChargingLeds do
  @moduledoc """
  Sweeps the five player LEDs while the controller charges, and leaves the
  middle one lit the rest of the time.

  Sony's firmware animates the player LEDs when the pad is on USB; over
  Bluetooth nothing does, so this is ours. The sweep is 1-3-5-2-4 and comes
  back to the middle LED (xx3xx) — which is also the kernel's "player 1"
  pattern, so handing the LEDs to anyone else looks unchanged.
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config}
  alias Padwave.Output.Hid

  @tick_ms 220
  # 1-3-5-2-4, then home to the middle LED.
  @sweep [0b00001, 0b00100, 0b10000, 0b00010, 0b01000, 0b00100]
  @home 0b00100

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc false
  def sweep, do: @sweep
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
    Hid.player_leds(Enum.at(@sweep, state.index))
    timer = Process.send_after(self(), :tick, @tick_ms)
    {:noreply, %{state | index: rem(state.index + 1, length(@sweep)), timer: timer}}
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
