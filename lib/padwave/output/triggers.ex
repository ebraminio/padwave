defmodule Padwave.Output.Triggers do
  @moduledoc """
  Trigger-actuator feedback while the triggers are being used.

  The DualSense has an actuator inside each trigger, separate from the two grip
  motors. It is the impulse-trigger idea from the Xbox pad, and on the DualSense
  it is driven through the adaptive-trigger block of the output report: mode
  `0x06` ("automatic"/vibration) takes `[frequency, force, start]`, and mode
  `0x00` turns it off.

  padwave buzzes the trigger that is currently changing the volume, so holding
  R2 to turn the music up feels like turning a detented knob rather than a
  silent press. It stops on release, and on disconnect nothing is left buzzing.

  This deliberately does *not* touch the grip motors: LinuxAudio4Dualsense5
  drives those as a subwoofer for the controller speaker.
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config}
  alias Padwave.Output.Hid

  @off 0x00
  @vibration 0x06

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc "Buzzes one trigger for `ms`, for tuning the feel from the UI."
  def test(side, ms \\ 500, mode \\ @vibration, params \\ nil) when side in [:left, :right] do
    GenServer.cast(__MODULE__, {:test, side, ms, mode, params})
  end

  @impl true
  def init(_opts) do
    Bus.subscribe()
    {:ok, %{left: false, right: false}}
  end

  @impl true
  def handle_info({:padwave, {:input, {:trigger, side, :pressed}}}, state) do
    if Config.get(:trigger_effects, true) do
      vibrate(side, :on)
    end

    {:noreply, Map.put(state, side, true)}
  end

  def handle_info({:padwave, {:input, {:trigger, side, :released}}}, state) do
    vibrate(side, :off)
    {:noreply, Map.put(state, side, false)}
  end

  def handle_cast({:test, side, ms, mode, params}, state) do
    Hid.trigger(side, mode, params || default_params())
    Process.send_after(self(), {:test_off, side}, ms |> max(50) |> min(10_000))
    {:noreply, state}
  end

  def handle_info({:test_off, side}, state) do
    vibrate(side, :off)
    {:noreply, state}
  end

  def handle_info({:padwave, {:device, :down, _info}}, state) do
    # The controller is gone; nothing to switch off, but forget the state so a
    # reconnect starts clean.
    {:noreply, %{left: false, right: false}}
  end

  def handle_info(_other, state), do: {:noreply, state}

  defp vibrate(side, :on), do: Hid.trigger(side, @vibration, default_params())

  defp default_params do
    [
      Config.get(:trigger_effect_frequency, 10),
      Config.get(:trigger_effect_force, 80),
      Config.get(:trigger_effect_start, 15)
    ]
  end

  defp vibrate(side, :off), do: Hid.trigger(side, @off, [0])
end
