defmodule Padwave.Input.Mapper do
  @moduledoc """
  Turns raw controller frames into a small, stable set of actions.

  This is where taste lives: which button does what. The layout below is built
  around a controller whose ✕ is dead and whose sticks are noisy, so the
  comfortable, reliable inputs (d-pad, L1/R1, L2/R2, face buttons) do the work
  and the sticks are not used at all.

      L2 (analogue)     volume down, deeper = faster
      R2 (analogue)     volume up
      d-pad ← / →       seek back / forward; hold to steer, ramping up
      d-pad ↑           next random station
      d-pad ↓           previously played station
      △  triangle       pause / resume
      ○  circle         play / resume   (stand-in for the broken ✕)
      □  square         random station from a different game
      L1 / R1           previous / next station
      Create / Options  previous / next station
      touchpad click    mute / unmute
      PS (hold ~2 s)    suspend everything and power the controller off
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config}
  alias Padwave.Input.Frame

  @bindings %{
    triangle: :toggle_pause,
    circle: :play,
    square: :shuffle_game,
    l1: :previous,
    r1: :next_random,
    create: :previous,
    options: :next_random,
    touchpad: :toggle_mute,
    mute: :toggle_mute,
    l3: :next_random,
    r3: :previous
  }

  @seek_tap_seconds 6
  @seek_hold_steps [10, 20, 40, 60]
  @seek_hold_intervals [320, 260, 190, 130]
  @ps_hold_ms 2000

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc "Latest frame plus a little context, for the UI."
  def snapshot, do: GenServer.call(__MODULE__, :snapshot, 2000)

  @impl true
  def init(_opts) do
    Bus.subscribe()
    Bus.subscribe(:frames)

    now = System.monotonic_time(:millisecond)

    {:ok,
     %{
       frame: nil,
       prev: nil,
       prev_buttons: [],
       prev_dpad: :none,
       last_activity: now,
       last_l2: now,
       last_r2: now,
       analog_resets: 0,
       last_action: nil,
       last_action_at: nil,
       last_shake: now,
       sim_depth: 0
     }}
  end

  @impl true
  def handle_call(:snapshot, _from, state) do
    {:reply,
     %{
       frame: state.frame,
       at: state.frame && state.frame.at,
       last_action: state.last_action,
       last_action_at: state.last_action_at,
       bindings: @bindings
     }, state}
  end

  @impl true
  def handle_info({:frame, _frame}, %{sim_depth: depth} = state) when depth > 0,
    do: {:noreply, state}

  def handle_info({:frame, frame}, state), do: handle_frame(frame, state)
  def handle_info({:padwave, {:frame, frame}}, state), do: handle_frame(frame, state)
  def handle_info({:sim_begin}, state), do: {:noreply, %{state | sim_depth: state.sim_depth + 1}}
  def handle_info({:sim_frame, frame}, state), do: handle_frame(frame, state)

  def handle_info({:sim_end}, state),
    do: {:noreply, %{state | sim_depth: max(state.sim_depth - 1, 0)}}

  def handle_info({:hold, direction, step}, state) do
    frame = state.frame

    if frame && frame.dpad == direction do
      interval = Enum.at(@seek_hold_intervals, min(step, length(@seek_hold_intervals) - 1))
      Process.send_after(self(), {:hold, direction, step + 1}, interval)
      {:noreply, emit({:seek, sign(direction) * Enum.at(@seek_hold_steps, min(step, 3))}, state)}
    else
      {:noreply, state}
    end
  end

  def handle_info({:ps_hold}, state) do
    if state.frame && Frame.pressed?(state.frame, :ps) do
      {:noreply, emit(:suspend, state)}
    else
      {:noreply, state}
    end
  end

  def handle_info({:padwave, {:device, :down, _info}}, state) do
    {:noreply, %{state | frame: nil, prev: nil, prev_buttons: [], prev_dpad: :none}}
  end

  def handle_info(_other, state), do: {:noreply, state}

  defp handle_frame(frame, state) do
    now = System.monotonic_time(:millisecond)

    state =
      cond do
        Frame.real_activity?(state.prev, frame) -> maybe_activity(now, state, :real)
        Frame.analog_activity?(state.prev, frame) -> maybe_activity(now, state, :analog)
        true -> state
      end

    state = detect_shake(now, frame, state)
    state = emit_new_buttons(state.prev_buttons, frame, state)
    state = handle_dpad(state.prev_dpad, frame, state)
    state = handle_analog(now, frame, state)

    {:noreply,
     %{
       state
       | frame: frame,
         prev: frame,
         prev_buttons: frame.buttons,
         prev_dpad: frame.dpad
     }}
  end

  # -- shake -----------------------------------------------------------------

  # AntennaPod resets its sleep timer when the phone is shaken harder than
  # 2.25 g. Same rule here, using the controller's accelerometer: the timer is
  # reset (any activity resets it) and the lightbar/UI acknowledge it.
  defp detect_shake(now, frame, state) do
    threshold = Config.get(:shake_g_force, 2.25)

    if frame.g_force >= threshold and now - state.last_shake > 1_500 do
      Logger.debug("[input] shake detected (#{frame.g_force} g)")
      Bus.broadcast({:input, :shake})
      state = maybe_activity(now, state, :real)
      %{state | last_shake: now, last_action: :shake, last_action_at: now}
    else
      state
    end
  end

  # -- buttons ---------------------------------------------------------------

  defp emit_new_buttons(prev_buttons, frame, state) do
    case frame.buttons -- prev_buttons do
      [] ->
        state

      pressed ->
        Enum.reduce(pressed, state, fn
          :ps, acc ->
            Process.send_after(self(), {:ps_hold}, @ps_hold_ms)
            acc

          button, acc ->
            case Map.get(@bindings, button) do
              nil -> acc
              action -> emit(action, acc)
            end
        end)
    end
  end

  # -- d-pad -----------------------------------------------------------------

  defp handle_dpad(prev_dpad, frame, state) do
    if frame.dpad == prev_dpad do
      state
    else
      case frame.dpad do
        :left ->
          Process.send_after(self(), {:hold, :left, 0}, Config.get(:dpad_hold_delay_ms, 420))
          emit({:seek, -@seek_tap_seconds}, state)

        :right ->
          Process.send_after(self(), {:hold, :right, 0}, Config.get(:dpad_hold_delay_ms, 420))
          emit({:seek, @seek_tap_seconds}, state)

        :up ->
          emit(:next_random, state)

        :down ->
          emit(:previous, state)

        _ ->
          state
      end
    end
  end

  defp sign(:left), do: -1
  defp sign(:right), do: 1

  # -- analogue triggers -----------------------------------------------------

  defp handle_analog(now, frame, state) do
    state
    |> trigger(now, :l2, frame.l2, -1)
    |> trigger(now, :r2, frame.r2, 1)
  end

  defp trigger(state, now, which, value, direction) do
    last_key = :"last_#{which}"
    last = Map.get(state, last_key, 0)
    deadzone = Config.get(:trigger_deadzone, 30)
    repeat = Config.get(:trigger_repeat_ms, 110)

    if value >= deadzone and now - last >= repeat do
      step =
        cond do
          value >= 220 -> 4
          value >= 150 -> 2
          true -> 1
        end

      emit({:volume, direction * step}, Map.put(state, last_key, now))
    else
      state
    end
  end

  # -- activity --------------------------------------------------------------

  # Analogue movement may reset the sleep timer, but only a bounded number of
  # times in a row: a dirty stick that drifts on its own must not keep the
  # radio awake forever. Any real button/d-pad/trigger press clears the count.
  defp maybe_activity(now, state, cause) do
    cap = Config.get(:analog_wake_cap, 6)

    cond do
      cause == :analog and state.analog_resets >= cap ->
        state

      now - state.last_activity > 900 ->
        Bus.broadcast({:input, :activity})

        resets = if cause == :analog, do: state.analog_resets + 1, else: 0
        %{state | last_activity: now, analog_resets: resets}

      cause == :real ->
        %{state | analog_resets: 0}

      true ->
        state
    end
  end

  # -- action plumbing -------------------------------------------------------

  defp emit(action, state) do
    Logger.debug("[input] #{inspect(action)}")
    Bus.broadcast({:action, action})
    %{state | last_action: action, last_action_at: System.monotonic_time(:millisecond)}
  end

  @doc "The button bindings, for documentation and the UI."
  def bindings, do: @bindings
end
