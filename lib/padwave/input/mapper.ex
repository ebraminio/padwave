defmodule Padwave.Input.Mapper do
  @moduledoc """
  Holds the latest frame and the button bindings for the UI.

  The controller logic itself lives in `padwave input` now: d-pad steering,
  button actions, the PS hold, the volume ramps, activity/wake, shake, the jack
  debounce and the trigger actuator events all arrive as bus events. This
  process only keeps the frame snapshot the panel reads and records the last
  action; `jack?/0` exists because the radio's resume path asks it.
  """

  use GenServer

  alias Padwave.Bus

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
    left_paddle: :previous,
    right_paddle: :next_random,
    fn1: :toggle_pause,
    fn2: :toggle_mute
  }

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc "The button bindings, for documentation and the UI."
  def bindings, do: @bindings

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
    {:ok, %{frame: nil, last_action: nil, last_action_at: nil, sim_depth: 0}}
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

  def handle_info({:frame, frame}, state), do: {:noreply, %{state | frame: frame}}
  def handle_info({:padwave, {:frame, frame}}, state), do: {:noreply, %{state | frame: frame}}

  def handle_info({:padwave, {:action, action}}, state),
    do: {:noreply, %{state | last_action: action,
                     last_action_at: System.monotonic_time(:millisecond)}}

  def handle_info({:sim_begin}, state), do: {:noreply, %{state | sim_depth: state.sim_depth + 1}}

  def handle_info({:sim_end}, state),
    do: {:noreply, %{state | sim_depth: max(state.sim_depth - 1, 0)}}

  def handle_info({:padwave, {:device, :down, _info}}, state),
    do: {:noreply, %{state | frame: nil}}

  def handle_info(_other, state), do: {:noreply, state}
end
