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
    Bus.subscribe()

    tracks = Library.scan(Config.music_roots())
    by_id = Map.new(tracks, fn track -> {track.id, track} end)
    persisted = Store.load()

    current = persisted["track_id"] && Map.get(by_id, persisted["track_id"])
    volume = persisted["volume"] || Config.get(:default_volume, 80)
    resume_pending = persisted["resume_pending"] == true

    Player.set_volume(volume)

    Logger.info("[radio] #{length(tracks)} stations in #{inspect(Config.music_roots())}")

    {:ok,
     %{
       tracks: tracks,
       by_id: by_id,
       current: current,
       volume: volume,
       muted: false,
       volume_before_mute: nil,
       volume_before_warning: nil,
       idle_warning: false,
       paused: true,
       playing: false,
       position: persisted["position"] || 0.0,
       duration: nil,
       suspended: false,
       resume_pending: resume_pending,
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

  def handle_cast(:warn_idle, state) do
    if state.idle_warning do
      {:noreply, state}
    else
      Logger.info("[radio] idle warning — raising volume")
      Bus.broadcast({:session, :warning})

      state =
        state
        |> Map.put(:volume_before_warning, state.volume)
        |> Map.put(:idle_warning, true)
        |> set_volume(Config.get(:idle_warn_volume, 100))

      {:noreply, state}
    end
  end

  def handle_cast(:unwarn_idle, state) do
    state =
      if state.idle_warning do
        Bus.broadcast({:session, :active})
        restore = state.volume_before_warning || Config.get(:default_volume, 80)

        state
        |> Map.put(:idle_warning, false)
        |> Map.put(:volume_before_warning, nil)
        |> set_volume(restore)
      else
        state
      end

    {:noreply, state}
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

    if state.resume_pending and state.current != nil do
      Process.send_after(self(), :resume, 1200)
    end

    {:noreply, state}
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

  def handle_info(:resume, state) do
    cond do
      state.current == nil ->
        {:noreply, state}

      not Padwave.Input.Device.connected?() ->
        # The sink takes a moment to come back after the controller does.
        Process.send_after(self(), :resume, 800)
        {:noreply, state}

      true ->
        Logger.info(
          "[radio] resuming #{state.current.title} at #{Float.round(state.position, 1)}s"
        )

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

  defp play_or_resume(%{playing: true} = state), do: state

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
    Player.set_volume(state.volume)

    state =
      state
      |> Map.put(:current, track)
      |> Map.put(:position, start * 1.0)
      |> Map.put(:duration, nil)
      |> Map.put(:playing, true)
      |> Map.put(:paused, false)
      |> Map.put(:resume_pending, false)
      |> Map.put(:suspended, false)
      |> Map.update!(:history, fn history ->
        case state.current do
          nil -> history
          %Track{id: id} -> if id == track.id, do: history, else: Enum.take([id | history], 40)
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

  defp seek(state, delta) when is_number(delta) do
    target = clamp(state.position + delta, 0, state.duration || 1.0e12)

    case Player.seek(target) do
      {:ok, _} -> %{state | position: target}
      _ -> state
    end
  end

  defp seek(state, _other), do: state

  defp set_volume(state, value) when is_number(value) do
    value = value |> round() |> clamp(0, 100)
    if value != state.volume, do: Player.set_volume(value)

    state
    |> Map.put(:volume, value)
    |> Map.put(:muted, value == 0)
    |> persist()
  end

  defp toggle_mute(%{muted: true} = state) do
    restore = state.volume_before_mute || Config.get(:default_volume, 80)
    set_volume(%{state | muted: false, volume_before_mute: nil}, restore)
  end

  defp toggle_mute(state) do
    set_volume(%{state | volume_before_mute: state.volume}, 0)
  end

  defp suspend(%{suspended: true} = state, _reason), do: state

  defp suspend(state, reason) do
    position = state.position

    if state.playing do
      Player.stop(fade: 300)
    end

    Bus.broadcast({:radio, {:suspended, reason}})

    state
    |> Map.put(:playing, false)
    |> Map.put(:paused, true)
    |> Map.put(:suspended, true)
    |> Map.put(:resume_pending, state.current != nil)
    |> Map.put(:position, position)
    |> persist()
  end

  # -- persistence -----------------------------------------------------------

  defp persist(state) do
    Store.save(%{
      "track_id" => state.current && state.current.id,
      "position" => Float.round(state.position * 1.0, 2),
      "volume" => state.volume,
      "paused" => state.paused,
      "resume_pending" => state.resume_pending,
      "history" => state.history
    })

    state
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
end
