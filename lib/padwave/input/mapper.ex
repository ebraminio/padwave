defmodule Padwave.Input.Mapper do
  @moduledoc """
  Turns raw controller frames into a small, stable set of actions.

  This is where taste lives: which button does what. The layout below is built
  around a controller whose ✕ is dead and whose sticks are noisy, so the
  comfortable, reliable inputs (d-pad, L1/R1, L2/R2, face buttons) do the work
  and the sticks are not used at all.

      L2 (analogue)     volume down, deeper = faster
      R2 (analogue)     volume up
      d-pad             handled by `padwave input` (CLI): ←/→ seek and steer,
                        ↑ next random, ↓ previous; the actions arrive here as
                        ordinary bus events
      △  triangle       pause / resume
      ○  circle         play / resume   (stand-in for the broken ✕)
      □  square         random station from a different game
      L1 / R1           previous / next station
      Create / Options  previous / next station
      touchpad click    mute / unmute
      Edge paddles      previous / next station
      Edge Fn buttons   pause / mute
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
    r3: :previous,
    # Edge only (standard pads report these as zero): the back paddles skip
    # stations, the two Fn buttons pause and mute.
    left_paddle: :previous,
    right_paddle: :next_random,
    fn1: :toggle_pause,
    fn2: :toggle_mute
  }

  @ps_hold_ms 2000

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc "Latest frame plus a little context, for the UI."
  def snapshot, do: GenServer.call(__MODULE__, :snapshot, 2000)

  @doc """
  Whether a headset is in the jack: `true`, `false`, or `nil` before the first
  frame (so callers can tell "out" from "don't know yet").
  """
  def jack? do
    case snapshot() do
      %{frame: %{jack: jack}} -> jack
      _ -> nil
    end
  end

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
       last_activity: now,
       last_l2: now,
       last_r2: now,
       analog_resets: 0,
       l2_down: false,
       r2_down: false,
       jack: nil,
       jack_pending: nil,
       jack_since: nil,
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

  def handle_info({:ps_hold}, state) do
    if state.frame && Frame.pressed?(state.frame, :ps) do
      {:noreply, emit(:suspend, state)}
    else
      {:noreply, state}
    end
  end

  def handle_info({:padwave, {:device, :down, _info}}, state) do
    # Forget the jack: the next controller should be judged on its own first
    # report, not on what was plugged into the previous one.
    state = %{state | jack: nil, jack_pending: nil, jack_since: nil}
    {:noreply, %{state | frame: nil, prev: nil, prev_buttons: []}}
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
    state = handle_analog(now, frame, state)
    state = track_trigger(:left, frame.l2, state)
    state = track_trigger(:right, frame.r2, state)
    state = track_jack(state, frame, now)

    {:noreply,
     %{
       state
       | frame: frame,
         prev: frame,
         prev_buttons: frame.buttons
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

  # -- trigger press/release -------------------------------------------------

  # Announces when a trigger crosses into use, with hysteresis, so the trigger
  # actuator can buzz exactly while the trigger is doing something.
  @trigger_down 30
  @trigger_up 15

  defp track_trigger(side, value, state) do
    key = if side == :left, do: :l2_down, else: :r2_down
    down? = Map.fetch!(state, key)

    cond do
      not down? and value >= @trigger_down ->
        Bus.broadcast({:input, {:trigger, side, :pressed}})
        Map.put(state, key, true)

      down? and value < @trigger_up ->
        Bus.broadcast({:input, {:trigger, side, :released}})
        Map.put(state, key, false)

      true ->
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

  # -- headset jack ----------------------------------------------------------

  # The jack is the output path: pulling the headset means the music would play
  # to an empty plug (the membrane is muted while something is inserted), and
  # plugging it back in is a clear sign you want to hear it again.
  #
  # A worn jack can flicker for a frame or two, so a change only counts once it
  # has held for @jack_debounce_ms. The actions are separate from :pause/:play so
  # the radio can resume only what the jack itself paused.
  @jack_debounce_ms 250

  defp track_jack(state, frame, now) do
    cond do
      state.jack == frame.jack ->
        %{state | jack_since: nil, jack_pending: nil}

      state.jack_pending != frame.jack or state.jack_since == nil ->
        %{state | jack_pending: frame.jack, jack_since: now}

      now - state.jack_since >= @jack_debounce_ms ->
        action = if frame.jack, do: :jack_plugged, else: :jack_unplugged
        Logger.info("[input] headset #{if frame.jack, do: "plugged in", else: "unplugged"}")
        Bus.broadcast({:action, action})
        %{state | jack: frame.jack, jack_pending: nil, jack_since: nil}

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
