defmodule Padwave.Player do
  @moduledoc """
  Crossfading transport on top of two `mpv` slots.

  Volume is applied per slot as `master * gain`, where `gain` is ramped by a
  40 ms ticker. That is what makes both the trigger-driven volume changes and
  the station crossfades smooth: nothing is ever cut, it is faded.
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config}
  alias Padwave.Player.Slot

  @tick_ms 40

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  def play(path, opts \\ []), do: GenServer.call(__MODULE__, {:play, path, opts}, 10_000)

  def pause(paused?), do: GenServer.cast(__MODULE__, {:pause, paused?})

  def set_volume(value) when is_number(value),
    do: GenServer.cast(__MODULE__, {:volume, value})

  def seek(seconds) when is_number(seconds),
    do: GenServer.call(__MODULE__, {:seek, seconds}, 5000)

  @doc """
  Jumps to `seconds` the same way a station change works: the idle slot starts
  at the new position while the current one keeps playing, and the two are
  crossfaded. Big jumps (an hour into the mix, say) stay continuous instead of
  cutting.
  """
  def crossfade_seek(seconds, opts \\ []) when is_number(seconds),
    do: GenServer.call(__MODULE__, {:crossfade_seek, seconds, opts}, 10_000)

  def stop(opts \\ []), do: GenServer.cast(__MODULE__, {:stop, opts})

  # The mpv engine transitions through the radio (play/crossfade), so it has no
  # queue of its own; the call exists so the radio can stay engine-agnostic.
  def enqueue(_paths), do: :ok

  def position, do: GenServer.call(__MODULE__, :position, 2000)
  def duration, do: GenServer.call(__MODULE__, :duration, 2000)
  def state, do: GenServer.call(__MODULE__, :state, 2000)

  @impl true
  def init(_opts) do
    Process.flag(:trap_exit, true)
    {:ok, a} = Slot.start_link(name: :a, owner: self())
    {:ok, b} = Slot.start_link(name: :b, owner: self())

    {:ok,
     %{
       slots: %{a: a, b: b},
       active: nil,
       fading_from: nil,
       stopping: false,
       path: nil,
       position: 0.0,
       duration: nil,
       paused: false,
       master: Config.get(:default_volume, 80) * 1.0,
       master_ramp: nil,
       gain: %{a: 0.0, b: 0.0},
       ramps: %{},
       applied: %{a: -1, b: -1},
       tick: nil,
       last_position_broadcast: now()
     }}
  end

  @impl true
  def handle_call({:play, path, opts}, _from, state) do
    fade = Keyword.get(opts, :fade, Config.get(:crossfade_ms, 1500))
    start = Keyword.get(opts, :start) || 0
    {:reply, :ok, do_play(state, path, start, fade)}
  end

  def handle_call({:seek, seconds}, _from, state) do
    case state.active do
      nil ->
        {:reply, {:error, :idle}, state}

      slot ->
        result = Slot.seek(state.slots[slot], seconds)
        state = if match?({:ok, _}, result), do: %{state | position: seconds * 1.0}, else: state
        {:reply, result, state}
    end
  end

  def handle_call({:crossfade_seek, seconds, opts}, _from, state) do
    fade = Keyword.get(opts, :fade, Config.get(:seek_crossfade_ms, 600))

    if state.path == nil do
      {:reply, {:error, :idle}, state}
    else
      {:reply, :ok, do_play(state, state.path, seconds, fade)}
    end
  end

  def handle_call(:position, _from, state), do: {:reply, state.position, state}
  def handle_call(:duration, _from, state), do: {:reply, state.duration, state}

  def handle_call(:state, _from, state) do
    {:reply,
     %{
       active: state.active,
       path: state.path,
       position: state.position,
       duration: state.duration,
       paused: state.paused,
       volume: round(state.master),
       gain: state.gain,
       ramps: Map.keys(state.ramps),
       applied: state.applied,
       fading_from: state.fading_from
     }, state}
  end

  @impl true
  def handle_cast({:pause, paused?}, state) do
    if state.active, do: Slot.pause(state.slots[state.active], paused?)
    {:noreply, %{state | paused: paused?}}
  end

  def handle_cast({:volume, value}, state) do
    target = value * 1.0

    state =
      %{state | master_ramp: ramp(state.master, target, now(), Config.get(:volume_fade_ms, 180))}
      |> start_tick()

    {:noreply, state}
  end

  def handle_cast({:stop, opts}, state) do
    fade = Keyword.get(opts, :fade, Config.get(:fade_out_ms, 450))

    case state.active do
      nil ->
        {:noreply, state}

      slot ->
        ramp = ramp(Map.get(state.gain, slot, 1.0), 0.0, now(), max(fade, 1))

        state =
          %{state | ramps: Map.put(state.ramps, slot, ramp), stopping: true, fading_from: nil}
          |> start_tick()

        {:noreply, state}
    end
  end

  @impl true
  def handle_info(:tick, state) do
    state = %{state | tick: nil}
    now = now()

    {master, master_ramp} = advance(state.master, state.master_ramp, now)

    {gains, ramps} =
      Enum.reduce(state.ramps, {state.gain, %{}}, fn {slot, ramp}, {gains, acc} ->
        done? = ramp_done?(ramp, now)
        gains = Map.put(gains, slot, if(done?, do: ramp.to, else: ramp_value(ramp, now)))
        {gains, if(done?, do: acc, else: Map.put(acc, slot, ramp))}
      end)

    state = %{state | master: master, master_ramp: master_ramp, gain: gains, ramps: ramps}
    state = push_volumes(state)

    state =
      cond do
        master_ramp != nil or map_size(ramps) > 0 -> start_tick(state)
        state.stopping -> finish_stop(state)
        state.fading_from != nil -> finish_crossfade(state)
        true -> state
      end

    {:noreply, state}
  end

  def handle_info({:slot, _name, {:poll, :position, seconds}}, state) do
    if state.active != nil do
      now = now()

      state =
        if now - state.last_position_broadcast > 900 do
          Bus.broadcast({:player, {:position, seconds}})
          %{state | last_position_broadcast: now}
        else
          state
        end

      {:noreply, %{state | position: seconds}}
    else
      {:noreply, state}
    end
  end

  def handle_info({:slot, name, {:poll, :duration, seconds}}, state) do
    if name == state.active do
      Bus.broadcast({:player, {:duration, seconds}})
      {:noreply, %{state | duration: seconds}}
    else
      {:noreply, state}
    end
  end

  def handle_info({:slot, name, {:end_file, "eof"}}, state) do
    if name == state.active do
      Bus.broadcast({:player, :eof})
    end

    {:noreply, %{state | paused: true}}
  end

  def handle_info({:slot, _name, {:end_file, "stop"}}, state), do: {:noreply, state}

  def handle_info({:slot, name, {:end_file, reason}}, state) do
    Logger.warning("[player] slot #{name} ended: #{inspect(reason)}")
    if name == state.active, do: Bus.broadcast({:player, {:error, reason}})
    {:noreply, state}
  end

  def handle_info({:slot, _name, {:loaded, path}}, state) do
    Logger.debug("[player] loaded #{path}")
    {:noreply, %{state | path: path}}
  end

  def handle_info({:slot, _name, _other}, state), do: {:noreply, state}

  def handle_info({:EXIT, pid, reason}, state) do
    case Enum.find(state.slots, fn {_name, slot} -> slot == pid end) do
      nil ->
        {:noreply, state}

      {name, _pid} ->
        Logger.error("[player] slot #{name} crashed: #{inspect(reason)} — restarting")
        {:ok, new_pid} = Slot.start_link(name: name, owner: self())

        # If the slot that died was the one playing, this is a transport
        # restart, not a state change: reload what it had and put its gain
        # straight back to 1.0. Leaving it at 0.0 made the radio look like it
        # was playing while mpv sat at volume zero and stopped reporting its
        # position.
        was_active = state.active == name

        state = %{
          state
          | slots: Map.put(state.slots, name, new_pid),
            applied: Map.put(state.applied, name, -1),
            gain: Map.put(state.gain, name, if(was_active, do: 1.0, else: 0.0))
        }

        state =
          if was_active and state.path != nil do
            Slot.load(new_pid, state.path, start: state.position)
            Slot.pause(new_pid, false)
            state
          else
            state
          end

        Process.send_after(self(), :restart_ramps, 200)
        {:noreply, state}
    end
  end

  def handle_info(:restart_ramps, state), do: {:noreply, start_tick(state)}
  def handle_info(_other, state), do: {:noreply, state}

  # -- playing ---------------------------------------------------------------

  defp do_play(state, path, start, fade) do
    target = next_slot(state.active)
    previous = state.active
    now = now()

    Slot.load(state.slots[target], path, start: start)
    Slot.pause(state.slots[target], false)

    state = %{
      state
      | active: target,
        path: path,
        position: start * 1.0,
        duration: nil,
        paused: false,
        gain: Map.put(state.gain, target, if(fade > 0, do: 0.0, else: 1.0)),
        ramps:
          if(fade > 0,
            do: Map.put(state.ramps, target, ramp(0.0, 1.0, now, fade)),
            else: state.ramps
          )
    }

    state =
      if previous != nil and previous != target and fade > 0 do
        current = Map.get(state.gain, previous, 1.0)

        %{
          state
          | ramps: Map.put(state.ramps, previous, ramp(current, 0.0, now, fade)),
            fading_from: previous
        }
      else
        if previous != nil and previous != target do
          Slot.stop(state.slots[previous])
          %{state | gain: Map.put(state.gain, previous, 0.0)}
        else
          state
        end
      end

    Bus.broadcast({:player, {:playing, path}})
    start_tick(state)
  end

  defp next_slot(nil), do: :a
  defp next_slot(:a), do: :b
  defp next_slot(:b), do: :a

  defp finish_crossfade(state) do
    old = state.fading_from
    if old, do: Slot.stop(state.slots[old])

    %{state | fading_from: nil, gain: Map.put(state.gain, old, 0.0)}
  end

  defp finish_stop(state) do
    if state.active, do: Slot.stop(state.slots[state.active])

    Bus.broadcast({:player, :stopped})

    %{
      state
      | active: nil,
        path: nil,
        position: 0.0,
        duration: nil,
        stopping: false,
        gain: Map.from_keys([:a, :b], 0.0),
        applied: Map.from_keys([:a, :b], -1)
    }
  end

  # -- volume ----------------------------------------------------------------

  defp push_volumes(state) do
    Enum.reduce([:a, :b], state, fn slot, acc ->
      value = round(acc.master * Map.get(acc.gain, slot, 0.0))

      if Map.get(acc.applied, slot) == value do
        acc
      else
        Slot.volume(acc.slots[slot], value)
        %{acc | applied: Map.put(acc.applied, slot, value)}
      end
    end)
  end

  defp start_tick(%{tick: nil} = state) do
    %{state | tick: Process.send_after(self(), :tick, @tick_ms)}
  end

  defp start_tick(state), do: state

  # -- ramps -----------------------------------------------------------------

  defp ramp(from, to, started_at, duration_ms) do
    %{from: from * 1.0, to: to * 1.0, started_at: started_at, duration: max(duration_ms, 1)}
  end

  defp ramp_value(%{from: from, to: to, started_at: started_at, duration: duration}, now) do
    progress = min(max((now - started_at) / duration, 0.0), 1.0)
    from + (to - from) * progress
  end

  defp ramp_done?(%{started_at: started_at, duration: duration}, now),
    do: now >= started_at + duration

  defp advance(master, nil, _now), do: {master, nil}

  defp advance(_master, ramp, now) do
    if ramp_done?(ramp, now), do: {ramp.to, nil}, else: {ramp_value(ramp, now), ramp}
  end

  defp now, do: System.monotonic_time(:millisecond)
end
