defmodule Padwave.Player.Ffmpeg do
  @moduledoc """
  A playback engine that owns the whole path: ffmpeg decodes a file to PCM,
  padwave encodes Opus and writes the controller's `0x36` report itself.

  Compared with the mpv engine this is deliberately simpler and more direct:

  * **One writer.** There is no PipeWire sink and no audio service in the
    path, so nothing competes for `valid_flag0`/`valid_flag1` claims.
  * **The clock is the playhead.** Frames are sent every 10 ms from an absolute
    monotonic schedule, so a slow encode does not drift the position: the
    playhead is computed from elapsed time, and audio follows it.
  * **Volume is sample math**, applied to the PCM before encoding — the same
    cubic curve the mpv fix used, without mpv's quantisation floor.

  What it does not have yet: crossfades (a seek or a station change is a hard
  cut), gapless between files, and network sources. Those are the next steps,
  not mysteries: the frames are ordinary PCM here, so mixing two of them is
  arithmetic rather than IPC.

  Pacing details worth knowing:

  * ffmpeg is allowed to decode ahead; the Erlang port buffers whatever it
    writes and backpressure stops it if we fall behind.
  * Playback only starts after `@prebuffer` frames (100 ms) have arrived, so a
    cold start or a slow disk does not underrun on the first tick.
  * `pause` sends SIGSTOP to ffmpeg (no wasted decode, no unbounded buffer) and
    SIGCONT on resume, then re-bases the clock.
  """

  use GenServer

  require Logger

  alias Padwave.Output.{Audio, Hid}

  @prebuffer 10
  @frame_ms 10
  @frame_bytes 480 * 2 * 2
  @idle_poll_ms 20
  # Keep at most a couple of seconds of decoded audio in memory. Above the high
  # water mark ffmpeg is stopped and below the low one it is resumed; the port
  # would otherwise happily decode the whole file into the mailbox.
  @high_water 150
  @low_water 75
  @next_high_water 400
  @prefetch_seconds 2.0

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  def play(path, opts \\ []), do: GenServer.call(__MODULE__, {:play, path, opts}, 15_000)
  def pause(paused?), do: GenServer.cast(__MODULE__, {:pause, paused?})
  def set_volume(value) when is_number(value), do: GenServer.cast(__MODULE__, {:volume, value})
  def seek(seconds) when is_number(seconds), do: GenServer.call(__MODULE__, {:seek, seconds}, 10_000)

  @doc "Appends paths to the gapless queue (the radio manages the shuffle)."
  def enqueue(paths) when is_list(paths), do: GenServer.cast(__MODULE__, {:enqueue, paths})

  # One hard cut; the seam keeps the mpv engine's API so the radio does not care.
  def crossfade_seek(seconds, _opts \\ []), do: seek(seconds)

  def stop(opts \\ []), do: GenServer.cast(__MODULE__, {:stop, opts})
  def position, do: GenServer.call(__MODULE__, :position, 2_000)
  def duration, do: GenServer.call(__MODULE__, :duration, 2_000)
  def state, do: GenServer.call(__MODULE__, :state, 2_000)

  @impl true
  def init(_opts) do
    {:ok,
     %{
       path: nil,
       duration: nil,
       base: 0.0,
       started_at: nil,
       paused: false,
       paused_at: nil,
       paused_total: 0,
       volume: 80,
       port: nil,
       os_pid: nil,
       encoder: nil,
       buffer: <<>>,
       frames: :queue.new(),
       ticking: false,
       next_at: nil,
       sent: 0,
       underruns: 0,
       output: :jack,
       last_broadcast: now(),
       eof_sent: false,
       throttled: false,
       queue: [],
       next_path: nil,
       next_duration: nil,
       next_port: nil,
       next_os_pid: nil,
       next_buffer: <<>>,
       next_frames: :queue.new(),
       next_throttled: false
     }}
  end

  @impl true
  def handle_call({:play, path, opts}, _from, state) do
    state = shutdown(state)
    start = Keyword.get(opts, :start) || 0
    output = Keyword.get(opts, :output, :jack)

    state = %{
      state
      | path: path,
        duration: probe_duration(path),
        base: start * 1.0,
        started_at: now(),
        paused: false,
        paused_at: nil,
        paused_total: 0,
        output: output,
        buffer: <<>>,
        frames: :queue.new(),
        sent: 0,
        underruns: 0,
        last_broadcast: now(),
        eof_sent: false,
        throttled: false,
        queue: Keyword.get(opts, :queue, []),
        next_path: nil,
        next_duration: nil,
        next_port: nil,
        next_os_pid: nil,
        next_buffer: <<>>,
        next_frames: :queue.new(),
        next_throttled: false
    }

    state = if state.duration, do: broadcast_duration(state), else: state
    state = %{state | encoder: open_encoder()}
    {:reply, :ok, open_ffmpeg(state, start)}
  end

  def handle_call({:seek, seconds}, _from, state) do
    state = cancel_tick(state)
    state = close_ffmpeg(state)

    state = %{
      state
      | base: seconds * 1.0,
        started_at: now(),
        paused_total: 0,
        buffer: <<>>,
        frames: :queue.new(),
        sent: 0
    }

    state = open_ffmpeg(state, seconds)

    # A paused seek stays paused: the SIGSTOP happens again after the new
    # process exists.
    state = if state.paused, do: pause_ffmpeg(state, true), else: state
    {:reply, :ok, state}
  end

  def handle_call(:position, _from, state), do: {:reply, position_of(state), state}
  def handle_call(:duration, _from, state), do: {:reply, state.duration, state}

  def handle_call(:state, _from, state) do
    {:reply,
     %{
       engine: :ffmpeg,
       path: state.path,
       position: Float.round(position_of(state) * 1.0, 2),
       duration: state.duration,
       paused: state.paused,
       volume: round(state.volume),
       sent: state.sent,
       buffered: :queue.len(state.frames),
       underruns: state.underruns,
       output: state.output
     }, state}
  end

  @impl true
  def handle_cast({:pause, paused?}, state) do
    cond do
      paused? == state.paused ->
        {:noreply, state}

      paused? ->
        state = cancel_tick(state)
        state = pause_ffmpeg(state, true)
        {:noreply, %{state | paused: true, paused_at: now()}}

      true ->
        paused_for = now() - (state.paused_at || now())
        state = pause_ffmpeg(state, false)

        # Keep `started_at` and account the pause, so the playhead resumes where
        # it stopped instead of jumping by the length of the pause.
        {:noreply,
         %{
           state
           | paused: false,
             paused_at: nil,
             paused_total: state.paused_total + max(paused_for, 0)
         }}
    end
  end

  def handle_cast({:enqueue, paths}, state),
    do: {:noreply, %{state | queue: state.queue ++ paths}}

  def handle_cast({:volume, value}, state), do: {:noreply, %{state | volume: value}}

  def handle_cast({:stop, _opts}, state), do: {:noreply, shutdown(state)}

  @impl true
  def handle_info({port, {:data, chunk}}, %{next_port: port} = state) when not is_nil(port) do
    state = %{state | next_buffer: state.next_buffer <> chunk}
    {frames, buffer} = split_frames(state.next_buffer)
    queue = Enum.reduce(frames, state.next_frames, &:queue.in/2)
    state = %{state | next_buffer: buffer, next_frames: queue}

    # The prefetch decoder only needs to be a few seconds ahead; keep it from
    # decoding the whole file into memory before the current one ends.
    state =
      if :queue.len(state.next_frames) >= @next_high_water and not state.next_throttled do
        %{pause_next(state, true) | next_throttled: true}
      else
        state
      end

    {:noreply, state}
  end

  def handle_info({port, {:exit_status, status}}, %{next_port: port} = state) when not is_nil(port) do
    Logger.info("[ffmpeg engine] prefetched decoder finished (#{status})")
    {:noreply, %{state | next_port: nil, next_os_pid: nil, next_throttled: false}}
  end

  def handle_info({port, {:data, chunk}}, %{port: port} = state) do
    state = %{state | buffer: state.buffer <> chunk}
    {frames, buffer} = split_frames(state.buffer)
    queue = Enum.reduce(frames, state.frames, &:queue.in/2)
    state = %{state | buffer: buffer, frames: queue}

    state =
      if not state.ticking and :queue.len(state.frames) >= @prebuffer and not state.paused do
        begin_tick(state)
      else
        state
      end

    # Backpressure: stop the decoder once we are comfortably ahead. The check is
    # on the queue length, not only on the flag, so a stale flag cannot let a
    # decoder run to the end of the file.
    state =
      if :queue.len(state.frames) >= @high_water and not state.throttled do
        %{pause_ffmpeg(state, true) | throttled: true}
      else
        state
      end

    {:noreply, state}
  end

  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    Logger.info("[ffmpeg engine] decoder finished (#{status})")
    state = %{state | port: nil, os_pid: nil, ticking: false}

    # A file shorter than the prebuffer still deserves to play.
    state =
      if not state.paused and :queue.len(state.frames) > 0 do
        begin_tick(state)
      else
        state
      end

    {:noreply, state}
  end

  def handle_info(:tick, state) do
    state = %{state | ticking: false}

    state =
      cond do
        state.path == nil ->
          state

        state.paused ->
          state

        state.port == nil and :queue.is_empty(state.frames) and not :queue.is_empty(state.next_frames) ->
          state
          |> promote_next()
          |> broadcast_position()
          |> schedule_tick()

        state.port == nil and :queue.is_empty(state.frames) ->
          state
          |> broadcast_eof()
          |> cancel_tick()

        true ->
          state = maybe_prefetch(state)

          case :queue.out(state.frames) do
            {{:value, pcm}, queue} ->
              send_frame(state, pcm)

              state = %{state | frames: queue, sent: state.sent + 1}

              state =
                if state.throttled and :queue.len(queue) <= @low_water, do: resume_decoder(state), else: state

              state
              |> broadcast_position()
              |> schedule_tick()

            {:empty, _queue} ->
              # An underrun keeps the clock honest and just leaves a gap.
              %{state | underruns: state.underruns + 1}
              |> broadcast_position()
              |> schedule_tick(@idle_poll_ms)
          end
      end

    {:noreply, state}
  end

  def handle_info(_other, state), do: {:noreply, state}

  @impl true
  def terminate(_reason, state) do
    _ = shutdown(state)
    :ok
  end

  # -- the audio path ---------------------------------------------------------

  defp send_frame(state, pcm) do
    gain = gain(state.volume)
    pcm = if gain >= 0.999, do: pcm, else: scale(pcm, gain)
    opus = encode(state.encoder, pcm)
    report = Audio.build(opus, seq: rem(state.sent, 16), counter: rem(state.sent, 256), output: state.output)
    Hid.send_audio(report)
  end

  # (volume/100)^3, the curve the panel already feels: 50 is an eighth, 12 is
  # quiet but not silent.
  defp gain(volume), do: :math.pow(max(volume, 0) / 100, 3)

  defp scale(pcm, gain) do
    for <<sample::little-signed-16 <- pcm>>, into: <<>> do
      <<round(sample * gain)::little-signed-16>>
    end
  end

  defp encode(nil, _pcm), do: :binary.copy(<<0>>, 200)

  defp encode(port, pcm) do
    true = Port.command(port, pcm)

    receive do
      {^port, {:data, opus}} -> opus
    after
      500 -> :binary.copy(<<0>>, 200)
    end
  end

  # -- process plumbing -------------------------------------------------------

  defp open_ffmpeg(%{path: nil} = state), do: state

  defp open_ffmpeg(state, start) do
    ffmpeg = System.find_executable("ffmpeg") || raise "ffmpeg is not installed"

    args =
      ["-v", "error", "-nostdin"] ++
        if(start > 0.5, do: ["-ss", Float.to_string(start * 1.0)], else: []) ++
        ["-i", state.path, "-f", "s16le", "-ar", "48000", "-ac", "2", "-"]

    port = Port.open({:spawn_executable, ffmpeg}, [:binary, :exit_status, {:args, args}])

    os_pid =
      case Port.info(port, :os_pid) do
        {:os_pid, pid} -> pid
        _ -> nil
      end

    # A fresh process is not throttled; leaving the flag set from the previous
    # one meant a seek started a decoder nobody ever paused.
    %{state | port: port, os_pid: os_pid, throttled: false}
  end

  defp close_next_decoder(%{next_port: nil} = state), do: state

  defp close_next_decoder(state) do
    if Port.info(state.next_port), do: Port.close(state.next_port)

    %{
      state
      | next_port: nil,
        next_os_pid: nil,
        next_buffer: <<>>,
        next_frames: :queue.new(),
        next_throttled: false
    }
  end

  defp close_ffmpeg(%{port: nil} = state), do: state

  defp close_ffmpeg(state) do
    if Port.info(state.port), do: Port.close(state.port)
    %{state | port: nil, os_pid: nil}
  end

  defp resume_decoder(state) do
    %{pause_ffmpeg(state, false) | throttled: false}
  end

  defp pause_ffmpeg(%{os_pid: nil} = state, _paused?), do: state

  defp pause_ffmpeg(state, paused?) do
    signal = if paused?, do: "-STOP", else: "-CONT"
    System.cmd("kill", [signal, to_string(state.os_pid)], stderr_to_stdout: true)
    state
  rescue
    _error -> state
  end

  defp open_encoder do
    path = System.find_executable("padwave_opusdec") || Path.expand("~/.local/bin/padwave_opusdec")

    Port.open({:spawn_executable, path}, [:binary, :exit_status, packet: 4, args: ["-e"]])
  rescue
    error ->
      Logger.warning("[ffmpeg engine] no Opus encoder: #{Exception.message(error)}")
      nil
  end

  defp shutdown(state) do
    state = cancel_tick(state)
    state = close_ffmpeg(state)
    state = close_next_decoder(state)
    if state.encoder, do: Port.close(state.encoder)

    %{
      state
      | encoder: nil,
        path: nil,
        duration: nil,
        base: 0.0,
        started_at: nil,
        paused: false,
        paused_at: nil,
        paused_total: 0,
        frames: :queue.new(),
        buffer: <<>>,
        sent: 0,
        underruns: 0
    }
  end

  # -- clock ------------------------------------------------------------------

  defp position_of(%{path: nil}), do: 0.0

  defp position_of(state) do
    reference = if state.paused, do: state.paused_at || now(), else: now()
    elapsed = max(reference - (state.started_at || reference), 0)
    position = state.base + elapsed / 1000

    case state.duration do
      nil -> position
      duration -> min(position, duration)
    end
  end

  defp begin_tick(state) do
    send(self(), :tick)
    %{state | ticking: true, next_at: now()}
  end

  defp schedule_tick(state, delay \\ @frame_ms) do
    next = (state.next_at || now()) + delay
    wait = max(next - now(), 0)
    Process.send_after(self(), :tick, wait)
    %{state | ticking: true, next_at: next}
  end

  defp cancel_tick(state), do: %{state | ticking: false, next_at: nil}

  # The radio keeps its own playhead from these messages; mpv's engine
  # broadcasts them, so this one has to as well.
  defp broadcast_position(state) do
    if now() - state.last_broadcast > 500 do
      Padwave.Bus.broadcast({:player, {:position, position_of(state)}})
      %{state | last_broadcast: now()}
    else
      state
    end
  end

  defp broadcast_duration(state) do
    Padwave.Bus.broadcast({:player, {:duration, state.duration}})
    state
  end

  # Start decoding the next station a couple of seconds before this one ends,
  # so the frame queue never runs dry at the junction.
  defp maybe_prefetch(%{next_path: nil, queue: [next | rest]} = state) do
    remaining = (state.duration || 0) - position_of(state)

    if remaining <= @prefetch_seconds or state.port == nil do
      state = %{state | queue: rest, next_path: next, next_duration: probe_duration(next)}
      open_next_decoder(state)
    else
      state
    end
  end

  defp maybe_prefetch(state), do: state

  defp open_next_decoder(%{next_path: nil} = state), do: state

  defp open_next_decoder(state) do
    ffmpeg = System.find_executable("ffmpeg") || raise "ffmpeg is not installed"

    args = [
      "-v",
      "error",
      "-nostdin",
      "-i",
      state.next_path,
      "-f",
      "s16le",
      "-ar",
      "48000",
      "-ac",
      "2",
      "-"
    ]

    port = Port.open({:spawn_executable, ffmpeg}, [:binary, :exit_status, {:args, args}])

    os_pid =
      case Port.info(port, :os_pid) do
        {:os_pid, pid} -> pid
        _ -> nil
      end

    %{
      state
      | next_port: port,
        next_os_pid: os_pid,
        next_buffer: <<>>,
        next_frames: :queue.new(),
        next_throttled: false
    }
  end

  # The current station has run out and the next one is already decoded: hand
  # its frames straight to the sender, so there is no silence at the junction.
  defp promote_next(state) do
    state = close_ffmpeg(state)

    state = %{
      state
      | path: state.next_path,
        duration: state.next_duration,
        frames: state.next_frames,
        next_path: nil,
        next_duration: nil,
        next_port: nil,
        next_os_pid: nil,
        next_buffer: <<>>,
        next_frames: :queue.new(),
        next_throttled: false,
        base: 0.0,
        started_at: now(),
        paused_total: 0,
        sent: 0,
        eof_sent: false,
        last_broadcast: now(),
        underruns: 0
    }

    Padwave.Bus.broadcast({:player, {:track, state.path}})
    Padwave.Bus.broadcast({:player, {:duration, state.duration}})
    state
  end

  defp pause_next(%{next_os_pid: nil} = state, _paused?), do: state

  defp pause_next(state, paused?) do
    signal = if paused?, do: "-STOP", else: "-CONT"
    System.cmd("kill", [signal, to_string(state.next_os_pid)], stderr_to_stdout: true)
    state
  rescue
    _error -> state
  end

  defp broadcast_eof(%{eof_sent: true} = state), do: state

  defp broadcast_eof(state) do
    Padwave.Bus.broadcast({:player, :eof})
    %{state | eof_sent: true}
  end

  defp split_frames(buffer), do: split_frames(buffer, [])

  defp split_frames(buffer, acc) when byte_size(buffer) >= @frame_bytes do
    <<frame::binary-size(@frame_bytes), rest::binary>> = buffer
    split_frames(rest, [frame | acc])
  end

  defp split_frames(buffer, acc), do: {Enum.reverse(acc), buffer}

  defp probe_duration(path) do
    case System.find_executable("ffprobe") do
      nil ->
        nil

      ffprobe ->
        case System.cmd(ffprobe, ["-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
               stderr_to_stdout: true
             ) do
          {output, 0} ->
            case Float.parse(String.trim(output)) do
              {seconds, _} when seconds > 0 -> seconds
              _ -> nil
            end

          _ ->
            nil
        end
    end
  rescue
    _error -> nil
  end

  defp now, do: System.monotonic_time(:millisecond)
end
