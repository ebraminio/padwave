defmodule Padwave.Radio do
  @moduledoc """
  The radio itself: what is playing, what plays next, how loud it is, and
  where to pick up again after the controller has been away.

  All input actions land here; all playback is delegated to `Padwave.Player`;
  the resume point is mirrored into `Padwave.Store` so a power cut, an idle
  shutdown or a dead controller all end the same way: the station continues
  where it was.
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config, Library, Player, Store}
  alias Padwave.Library.Track

  # How often the playhead is written to disk while playing. A clean shutdown
  # also checkpoints in terminate/2.
  @checkpoint_ms 15_000

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  def status, do: GenServer.call(__MODULE__, :status, 2000)
  def tracks, do: GenServer.call(__MODULE__, :tracks, 2000)

  def command(action, args \\ %{}),
    do: GenServer.call(__MODULE__, {:command, action, args}, 10_000)

  def play_track(id, opts \\ []), do: GenServer.call(__MODULE__, {:play_track, id, opts}, 10_000)
  def next_random, do: GenServer.cast(__MODULE__, :next_random)
  def previous, do: GenServer.cast(__MODULE__, :previous)
  def toggle_pause, do: GenServer.cast(__MODULE__, :toggle_pause)
  def seek(delta), do: GenServer.cast(__MODULE__, {:seek, delta})
  def nudge_volume(delta), do: GenServer.cast(__MODULE__, {:nudge_volume, delta})
  def set_volume(value), do: GenServer.cast(__MODULE__, {:set_volume, value})
  def warn_idle, do: GenServer.cast(__MODULE__, :warn_idle)
  def unwarn_idle, do: GenServer.cast(__MODULE__, :unwarn_idle)
  def suspend(reason \\ :manual), do: GenServer.cast(__MODULE__, {:suspend, reason})

  @impl true
  def init(_opts) do
    # Trap exits so terminate/2 runs on a clean shutdown and can write the
    # exact playhead to disk before the VM goes away.
    Process.flag(:trap_exit, true)
    Bus.subscribe()

    tracks = Library.scan(Config.music_roots())
    by_id = Map.new(tracks, fn track -> {track.id, track} end)
    persisted = Store.load()

    current = persisted["track_id"] && Map.get(by_id, persisted["track_id"])

    # The library can come back empty (a bad root, an unmounted disk, a typo in
    # PADWAVE_ROOTS). If the saved track is missing, fall back to the most
    # recent thing in the history rather than forgetting what was playing.
    current =
      current ||
        (List.wrap(persisted["recent"]) ++ List.wrap(persisted["history"]))
        |> Enum.find_value(&Map.get(by_id, &1))

    if current && persisted["track_id"] != current.id do
      Logger.info("[radio] recovered #{current.title} from history")
    end

    volume = persisted["volume"] || Config.get(:default_volume, 80)

    # A service restart should feel like the radio was never away: if it was
    # playing, play again from the saved second; if it was paused, restore the
    # station and the playhead but stay paused.
    was_playing = persisted["playing"] == true
    resume_pending = persisted["resume_pending"] == true or was_playing
    paused = not resume_pending and Map.get(persisted, "paused", true) != false

    Player.set_volume(volume)

    Logger.info("[radio] #{length(tracks)} stations in #{inspect(Config.music_roots())}")

    if tracks == [] do
      Logger.warning("[radio] no audio found — check PADWAVE_ROOTS or ~/.config/padwave/env")
    end

    Process.send_after(self(), :checkpoint, @checkpoint_ms)

    if resume_pending and current do
      Logger.info("[radio] will resume #{current.title} at #{Float.round(state_position(persisted), 1)}s")
    end

    {:ok,
     %{
       tracks: tracks,
       by_id: by_id,
       current: current,
       last_track_id: persisted["track_id"],
       volume: volume,
       muted: false,
       volume_before_mute: nil,
       idle_warning: false,
       warn_at: nil,
       warn_factor: 1.0,
       playing: false,
       position: persisted["position"] || 0.0,
       duration: nil,
       suspended: false,
       suspended_by: nil,
       resume_pending: resume_pending,
       paused: paused,
       paused_by_jack: false,
       bags: %{},
       history: List.wrap(persisted["history"])
     }}
  end

  @impl true
  def handle_call(:status, _from, state), do: {:reply, status_map(state), state}

  def handle_call(:tracks, _from, state) do
    {:reply, Enum.map(state.tracks, &Track.to_json/1), state}
  end

  def handle_call({:play_track, id, opts}, _from, state) do
    case Map.get(state.by_id, id) do
      nil ->
        {:reply, {:error, :unknown_track}, state}

      track ->
        {:reply, :ok, play(state, track, opts)}
    end
  end

  def handle_call({:command, action, args}, _from, state) do
    {result, state} = dispatch(to_string(action), args, state)
    reply = if match?({:ok, _}, result), do: :ok, else: result
    {:reply, reply, state}
  end

  @impl true
  def handle_cast(:next_random, state), do: {:noreply, play_random(state)}

  def handle_cast(:previous, state), do: {:noreply, go_back(state)}
  def handle_cast(:toggle_pause, state), do: {:noreply, toggle_pause(state)}

  def handle_cast({:seek, delta}, state), do: {:noreply, seek(state, delta)}

  def handle_cast({:nudge_volume, delta}, state),
    do: {:noreply, set_volume(state, state.volume + delta)}

  def handle_cast({:set_volume, value}, state), do: {:noreply, set_volume(state, value)}

  # Sleep timer, borrowed from AntennaPod: nothing happens for most of the
  # grace period, then the volume fades linearly to a floor over the last few
  # seconds and playback stops. It fades *down*, never up — the point is to
  # not wake anyone, and a sudden volume increase is the opposite of that.
  def handle_cast(:warn_idle, state) do
    if state.idle_warning do
      {:noreply, state}
    else
      Logger.info("[radio] sleep timer armed — volume will fade out at the end")
      Bus.broadcast({:session, :warning})
      Process.send_after(self(), :warn_tick, 250)
      {:noreply, %{state | idle_warning: true, warn_at: now(), warn_factor: 1.0}}
    end
  end

  def handle_cast(:unwarn_idle, state) do
    if state.idle_warning do
      Logger.info("[radio] sleep timer cancelled — volume restored")
      Bus.broadcast({:session, :active})
      {:noreply, apply_volume(%{state | idle_warning: false, warn_at: nil, warn_factor: 1.0})}
    else
      {:noreply, state}
    end
  end

  def handle_cast({:suspend, reason}, state), do: {:noreply, suspend(state, reason)}

  @impl true
  def handle_info({:padwave, {:action, action}}, state) do
    {_result, state} = dispatch_action(action, state)
    {:noreply, state}
  end

  def handle_info({:padwave, {:device, :up, info}}, state) do
    Logger.info("[radio] controller connected (#{info.mac})")
    state = %{state | suspended: false}

    # A DualSense that we powered off often reconnects by itself a few seconds
    # later. That is not the listener coming back, so an idle/manual shutdown
    # only resumes on a real input event; a link lost out of range still
    # resumes as soon as it returns.
    cond do
      state.suspended_by in [:idle, :manual] ->
        {:noreply, state}

      state.resume_pending and state.current != nil ->
        Process.send_after(self(), :resume, 1200)
        {:noreply, state}

      true ->
        {:noreply, state}
    end
  end

  # Any real input after an idle shutdown is the listener picking the pad back
  # up: pick the station up where it stopped.
  def handle_info({:padwave, {:input, :activity}}, state) do
    if state.suspended_by in [:idle, :manual] and Padwave.Input.Device.connected?() do
      Logger.info("[radio] woken by input — resuming")
      send(self(), :resume)
      {:noreply, %{state | suspended_by: nil, resume_pending: true}}
    else
      {:noreply, state}
    end
  end

  def handle_info({:padwave, {:device, :down, info}}, state) do
    Logger.info("[radio] controller disconnected (#{inspect(info[:reason])})")
    {:noreply, suspend(state, :disconnected)}
  end

  def handle_info({:padwave, {:player, {:position, seconds}}}, state) do
    {:noreply, %{state | position: seconds}}
  end

  def handle_info({:padwave, {:player, {:duration, seconds}}}, state) do
    {:noreply, %{state | duration: seconds}}
  end

  def handle_info({:padwave, {:player, {:playing, _path}}}, state) do
    {:noreply, %{state | playing: true, paused: false}}
  end

  def handle_info({:padwave, {:player, :eof}}, state) do
    Logger.info("[radio] station finished — picking another")
    {:noreply, play_random(state)}
  end

  def handle_info({:padwave, {:player, {:error, reason}}}, state) do
    Logger.warning("[radio] playback error: #{inspect(reason)}")
    {:noreply, %{state | playing: false, paused: true}}
  end

  def handle_info({:padwave, {:player, :stopped}}, state) do
    {:noreply, %{state | playing: false, paused: true}}
  end

  # Save the playhead every so often while playing; a clean shutdown also
  # checkpoints in terminate/2, so a crash costs at most one interval.
  def handle_info(:warn_tick, state) do
    if state.idle_warning and state.warn_at do
      grace = Config.get(:idle_warn_grace_ms, 60_000)
      fade = max(Config.get(:idle_fade_ms, 10_000), 1)
      floor = Config.get(:idle_fade_floor, 10) / 100.0
      remaining = grace - (now() - state.warn_at)

      factor = if remaining <= fade, do: max(remaining / fade, floor), else: 1.0

      state = apply_volume(%{state | warn_factor: factor})
      if remaining > 0, do: Process.send_after(self(), :warn_tick, 250)
      {:noreply, state}
    else
      {:noreply, state}
    end
  end

  def handle_info(:checkpoint, state) do
    Process.send_after(self(), :checkpoint, @checkpoint_ms)

    if state.playing do
      {:noreply, persist(state)}
    else
      {:noreply, state}
    end
  end

  def handle_info(:resume, state) do
    cond do
      state.current == nil ->
        {:noreply, state}

      not Padwave.Input.Device.connected?() ->
        # The sink takes a moment to come back after the controller does.
        Process.send_after(self(), :resume, 800)
        {:noreply, state}

      true ->
        Logger.info("[radio] resuming #{state.current.title} at #{Float.round(state.position, 1)}s")

        state =
          play(state, state.current,
            start: state.position,
            fade: Config.get(:fade_in_ms, 900)
          )

        {:noreply, %{state | resume_pending: false}}
    end
  end

  def handle_info(_other, state), do: {:noreply, state}

  # -- actions ---------------------------------------------------------------

  defp dispatch("pause", _args, state), do: {:ok, toggle_pause(state)}
  defp dispatch("play", _args, state), do: {:ok, play_or_resume(state)}
  defp dispatch("next", _args, state), do: {:ok, play_random(state)}
  defp dispatch("previous", _args, state), do: {:ok, go_back(state)}
  defp dispatch("shuffle_game", _args, state), do: {:ok, play_random(state, same_game: false)}
  defp dispatch("stop", _args, state), do: {:ok, suspend(state, :manual)}

  defp dispatch("seek", args, state),
    do: {:ok, seek(state, to_number(args["delta"] || args[:delta], 0))}

  defp dispatch("seek_to", args, state),
    do: {:ok, seek(state, to_number(args["position"], 0) - state.position)}

  defp dispatch("volume", args, state),
    do: {:ok, set_volume(state, to_number(args["value"], state.volume))}

  defp dispatch("nudge_volume", args, state),
    do: {:ok, set_volume(state, state.volume + to_number(args["delta"], 0))}

  defp dispatch("mute", _args, state), do: {:ok, toggle_mute(state)}
  defp dispatch("play_track", args, state), do: {:ok, play_id(state, args["id"])}
  defp dispatch("suspend", _args, state), do: {:ok, suspend(state, :manual)}
  defp dispatch(other, _args, state), do: {{:error, {:unknown_action, other}}, state}

  defp dispatch_action(:toggle_pause, state), do: {:ok, toggle_pause(state)}

  # Pause, but never resume: used when the headset is unplugged.
  defp dispatch_action(:pause, state) do
    if state.playing and not state.paused, do: {:ok, toggle_pause(state)}, else: {:ok, state}
  end

  # The jack paused it, so the jack can un-pause it. A pause the user made
  # themselves is not undone by plugging a cable in.
  defp dispatch_action(:jack_unplugged, state) do
    if state.playing and not state.paused do
      Logger.info("[radio] headphones out — pausing for the jack")
      {:ok, state |> Map.put(:paused_by_jack, true) |> toggle_pause()}
    else
      {:ok, state}
    end
  end

  defp dispatch_action(:jack_plugged, state) do
    if state.paused_by_jack and state.paused do
      Logger.info("[radio] headphones back — resuming")
      {:ok, state |> Map.put(:paused_by_jack, false) |> toggle_pause()}
    else
      {:ok, %{state | paused_by_jack: false}}
    end
  end

  defp dispatch_action(:play, state), do: {:ok, play_or_resume(state)}
  defp dispatch_action(:next_random, state), do: {:ok, play_random(state)}
  defp dispatch_action(:previous, state), do: {:ok, go_back(state)}
  defp dispatch_action(:shuffle_game, state), do: {:ok, play_random(state, same_game: false)}
  defp dispatch_action({:seek, delta}, state), do: {:ok, seek(state, delta)}

  defp dispatch_action({:volume, delta}, state),
    do: {:ok, set_volume(state, state.volume + delta)}

  defp dispatch_action(:toggle_mute, state), do: {:ok, toggle_mute(state)}
  defp dispatch_action(:suspend, state), do: {:ok, suspend(state, :manual)}
  defp dispatch_action(_other, state), do: {:ok, state}

  # -- transport -------------------------------------------------------------

  defp play_or_resume(%{playing: true, paused: false} = state), do: state

  # Playing but paused: "play" means unpause, not "already playing".
  defp play_or_resume(%{playing: true} = state), do: toggle_pause(state)

  defp play_or_resume(state) do
    cond do
      state.current != nil -> play(state, state.current, start: state.position)
      true -> play_random(state)
    end
  end

  defp toggle_pause(%{playing: false} = state), do: play_or_resume(state)

  defp toggle_pause(state) do
    paused = not state.paused
    Player.pause(paused)
    persist(%{state | paused: paused})
  end

  defp play_id(state, nil), do: state

  defp play_id(state, id) do
    case Map.get(state.by_id, id) do
      nil -> state
      track -> play(state, track)
    end
  end

  defp play(state, %Track{} = track, opts \\ []) do
    start = Keyword.get(opts, :start, 0) || 0
    fade = Keyword.get(opts, :fade, Config.get(:crossfade_ms, 1500))

    :ok = Player.play(track.path, start: start, fade: fade)
    apply_volume(state)

    state =
      state
      |> Map.put(:current, track)
      |> Map.put(:last_track_id, track.id)
      |> Map.put(:position, start * 1.0)
      |> Map.put(:duration, nil)
      |> Map.put(:playing, true)
      |> Map.put(:paused, false)
      |> Map.put(:resume_pending, false)
      |> Map.put(:suspended, false)
      |> Map.update!(:history, fn history ->
        case state.current do
          nil ->
            history

          %Track{id: id} ->
            cond do
              id == track.id -> history
              match?([^id | _], history) -> history
              true -> Enum.take([id | history], 40)
            end
        end
      end)

    Bus.broadcast({:radio, {:now_playing, Track.to_json(track)}})
    persist(state)
  end

  # Stations are played "bag style": every station of a folder is played once
  # before any of them repeats, so a folder behaves like a playlist rather than
  # a slot machine. Refilling the bag reshuffles it, which keeps the order
  # fresh without ever feeling random-with-duplicates.
  defp play_random(state, opts \\ []) do
    case take_next(state, Keyword.get(opts, :same_game, true)) do
      {nil, state} -> state
      {%Track{} = track, state} -> play(state, track)
    end
  end

  defp take_next(%{tracks: []} = state, _same_game), do: {nil, state}

  defp take_next(state, same_game) do
    game = pick_game(state, same_game)

    case take_from_bag(state, game) do
      {nil, state} -> if same_game, do: take_next(state, false), else: {nil, state}
      {id, state} -> {Map.get(state.by_id, id), state}
    end
  end

  defp pick_game(state, true) do
    case state.current do
      %Track{game: game} -> game
      nil -> state.tracks |> Enum.map(& &1.game) |> Enum.uniq() |> Enum.random()
    end
  end

  defp pick_game(state, false) do
    games = state.tracks |> Enum.map(& &1.game) |> Enum.uniq()
    others = games -- [state.current && state.current.game]
    Enum.random(if others == [], do: games, else: others)
  end

  defp take_from_bag(state, game) do
    pool = for track <- state.tracks, track.game == game, do: track.id
    current_id = state.current && state.current.id

    bag =
      state.bags
      |> Map.get(game, [])
      |> Enum.filter(&(&1 in pool))

    bag =
      if bag == [] do
        Enum.shuffle(pool -- [current_id])
      else
        bag
      end

    case bag do
      [] -> {nil, state}
      [id | rest] -> {id, %{state | bags: Map.put(state.bags, game, rest)}}
    end
  end

  defp go_back(state) do
    history = state.history -- [state.current && state.current.id]

    case Enum.find(history, fn id -> Map.has_key?(state.by_id, id) end) do
      nil ->
        play_random(state)

      id ->
        state = %{state | history: Enum.reject(state.history, &(&1 == id))}
        play(state, Map.fetch!(state.by_id, id))
    end
  end

  # Small steps are a plain seek (cheap, and mpv is gapless on local files).
  # Big jumps crossfade between the old and the new position using the idle
  # slot, exactly like a station change — so hopping from the first hour to the
  # second stays musical instead of cutting.
  defp seek(state, delta) when is_number(delta) do
    target = clamp(state.position + delta, 0, state.duration || 1.0e12)
    threshold = Config.get(:seek_crossfade_threshold, 20)

    cond do
      state.playing and abs(delta) >= threshold ->
        case Player.crossfade_seek(target, fade: Config.get(:seek_crossfade_ms, 600)) do
          :ok -> %{state | position: target}
          _ -> state
        end

      true ->
        case Player.seek(target) do
          {:ok, _} -> %{state | position: target}
          _ -> state
        end
    end
  end

  defp seek(state, _other), do: state

  defp set_volume(state, value) when is_number(value) do
    value = value |> round() |> clamp(0, 100)

    # Remember the last audible level whenever we hit zero, whichever way we
    # got there (mute button, slider, or holding L2), so unmuting always comes
    # back to something audible instead of the default.
    state =
      cond do
        value == 0 and state.volume > 0 -> %{state | volume_before_mute: state.volume}
        value > 0 and state.volume == 0 -> %{state | volume_before_mute: nil}
        true -> state
      end

    state =
      state
      |> Map.put(:volume, value)
      |> Map.put(:muted, value == 0)
      |> apply_volume()
      |> persist()

    Bus.broadcast({:radio, {:volume, value}})
    state
  end

  # The volume the listener asked for is `state.volume`; what actually reaches
  # the player is that times the sleep-timer fade.
  defp apply_volume(state) do
    Player.set_volume(round(state.volume * state.warn_factor))
    state
  end

  defp toggle_mute(%{muted: true} = state) do
    set_volume(state, state.volume_before_mute || Config.get(:default_volume, 80))
  end

  defp toggle_mute(state), do: set_volume(state, 0)

  defp suspend(%{suspended: true} = state, _reason), do: state

  defp suspend(state, reason) do
    position = state.position

    if state.playing do
      Player.stop(fade: 300)
    end

    Bus.broadcast({:radio, {:suspended, reason}})

    # Reset the sleep timer with the suspension: otherwise the fade factor
    # (0.1) would still be in effect when the station resumes, and the timer
    # could never arm again because idle_warning was left true.
    state
    |> Map.put(:playing, false)
    |> Map.put(:paused, true)
    |> Map.put(:suspended, true)
    |> Map.put(:suspended_by, reason)
    |> Map.put(:idle_warning, false)
    |> Map.put(:warn_at, nil)
    |> Map.put(:warn_factor, 1.0)
    |> Map.put(:resume_pending, state.current != nil)
    |> Map.put(:position, position)
    |> persist()
  end

  # -- persistence -----------------------------------------------------------

  defp persist(state) do
    Store.save(state_map(state))
    state
  end

  defp state_map(state) do
    %{
      # Keep the last known track even if `current` is nil (empty library), so a
      # bad scan cannot erase the resume point.
      "track_id" => (state.current && state.current.id) || state.last_track_id,
      "position" => Float.round(state.position * 1.0, 2),
      "volume" => state.volume,
      "paused" => state.paused,
      # `playing` means "should resume on boot"; a paused session must stay paused.
      "playing" => state.playing and not state.paused,
      "resume_pending" => state.resume_pending,
      "history" => state.history
    }
  end

  defp state_position(persisted), do: persisted["position"] || 0.0

  @impl true
  def terminate(_reason, state) do
    try do
      Store.save_now(state_map(state))
    rescue
      _ -> :ok
    catch
      _, _ -> :ok
    end
  end

  # -- status ----------------------------------------------------------------

  defp status_map(state) do
    %{
      current: state.current && Track.to_json(state.current),
      position: Float.round(state.position * 1.0, 1),
      duration: state.duration && Float.round(state.duration, 1),
      volume: state.volume,
      muted: state.muted,
      paused: state.paused,
      playing: state.playing,
      suspended: state.suspended,
      resume_pending: state.resume_pending,
      idle_warning: state.idle_warning,
      fade_percent: round(state.warn_factor * 100),
      history: state.history
    }
  end

  defp to_number(value, _default) when is_number(value), do: value

  defp to_number(value, default) when is_binary(value) do
    case Float.parse(value) do
      {number, _} -> number
      :error -> default
    end
  end

  defp to_number(_value, default), do: default

  defp clamp(value, min, max), do: value |> max(min) |> min(max)

  defp now, do: System.monotonic_time(:millisecond)
end
