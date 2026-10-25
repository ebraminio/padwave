defmodule Padwave.Session do
  @moduledoc """
  The idle watchdog.

  After 15 minutes without a real button press the radio raises the volume as
  a warning; one minute later it fades out, remembers the exact playhead and
  switches the controller off. Pressing anything during the warning cancels
  the shutdown and restores the previous volume. When the controller comes
  back, `Padwave.Radio` picks the same station up at the same second.

  Analogue sticks are not regarded as activity by default (see
  `Padwave.Input.Frame.activity?/2`) because this controller's sticks jitter
  on their own — that would defeat the whole point of going idle.
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config, Power, Radio}

  # AntennaPod's timer is second-accurate; ticking every second keeps our
  # arming and shutdown within a second of the configured times.
  @tick_ms 1_000

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  def status, do: GenServer.call(__MODULE__, :status, 2000)

  @impl true
  def init(_opts) do
    Bus.subscribe()
    Process.send_after(self(), :tick, @tick_ms)

    {:ok,
     %{
       phase: :active,
       last_activity: now(),
       warned_at: nil,
       connected: false
     }}
  end

  @impl true
  def handle_call(:status, _from, state) do
    timeout = Config.get(:idle_timeout_ms, 15 * 60 * 1000)
    grace = Config.get(:idle_warn_grace_ms, 60 * 1000)
    idle = div(now() - state.last_activity, 1000)

    {:reply,
     %{
       phase: state.phase,
       idle_seconds: idle,
       warning_in: max(div(timeout, 1000) - idle, 0),
       shutdown_in:
         if(state.phase == :warning,
           do: max(div(grace, 1000) - (idle - div(timeout, 1000)), 0),
           else: nil
         ),
       enabled: Config.get(:idle_enabled, true)
     }, state}
  end

  @impl true
  def handle_info(:tick, state) do
    Process.send_after(self(), :tick, @tick_ms)
    {:noreply, evaluate(state)}
  end

  def handle_info({:padwave, {:input, :activity}}, state) do
    state = %{state | last_activity: now()}

    state =
      if state.phase == :warning do
        Logger.info("[session] activity during idle warning — cancelling shutdown")
        Radio.unwarn_idle()
        %{state | phase: :active, warned_at: nil}
      else
        state
      end

    {:noreply, state}
  end

  def handle_info({:padwave, {:device, :up, _info}}, state) do
    # After an idle shutdown the controller tends to reconnect on its own.
    # Do not restart the countdown (and do not re-power it off) until the
    # listener actually touches it.
    phase = if state.phase == :suspended, do: :suspended, else: :active

    {:noreply, %{state | connected: true, phase: phase, last_activity: now()}}
  end

  def handle_info({:padwave, {:device, :down, _info}}, state) do
    {:noreply, %{state | connected: false, phase: :active, last_activity: now()}}
  end

  def handle_info(_other, state), do: {:noreply, state}

  defp evaluate(state) do
    cond do
      not Config.get(:idle_enabled, true) -> state
      not state.connected -> state
      true -> evaluate_idle(state)
    end
  end

  defp evaluate_idle(%{phase: :active} = state) do
    timeout = Config.get(:idle_timeout_ms, 15 * 60 * 1000)

    if now() - state.last_activity >= timeout do
      Radio.warn_idle()

      %{state | phase: :warning, warned_at: now()}
    else
      state
    end
  end

  defp evaluate_idle(%{phase: :warning} = state) do
    grace = Config.get(:idle_warn_grace_ms, 60 * 1000)

    if now() - state.warned_at >= grace do
      Logger.info("[session] idle timeout — stopping playback and powering the controller off")

      Radio.suspend(:idle)
      # bluetoothctl takes seconds; never block the watchdog (or the web
      # stream that asks it for its status) on it.
      Task.start(fn -> Power.disconnect() end)

      %{state | phase: :suspended}
    else
      state
    end
  end

  defp evaluate_idle(state), do: state

  defp now, do: System.monotonic_time(:millisecond)
end
