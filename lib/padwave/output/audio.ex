defmodule Padwave.Output.Audio do
  @moduledoc """
  Builds and sends a `0x36` audio report from padwave itself, for testing.

  The real `0x36` writer is LinuxAudio4Dualsense5: it owns the stream that keeps
  the speaker and the haptics fed, and two writers of the same report would
  fight at 90-odd packets a second. This module exists to answer a different
  question — *is a packet we build byte-for-byte valid?* — and it answers it by
  playing noise, because a packet that the controller plays is a packet the
  controller accepted.

  Use it with the audio service stopped: while that service is running, its own
  packet lands within ten milliseconds and overwrites this one.

  ## The packet

  The layout is the one in `Padwave.Schema.DualSenseAudio`, which is the same
  one the audio project builds in C:

      sub-packet 0x11  config: enable mask, haptic lengths, counter
      sub-packet 0x10  63-byte state snapshot (valid_flag0/1, volumes, routing)
      sub-packet 0x12  64 bytes of haptic PCM (zeroes here)
      sub-packet 0x13  speaker, or 0x16 for the jack: 200 bytes of Opus
      padding          50 bytes
      CRC-32           seed 0xA2, little-endian, over the previous 394 bytes

  The Opus frame is stereo 48 kHz, 480 samples, CBR 160 kbps — exactly 200 bytes,
  which is why the length byte is a constant.
  """

  import Bitwise

  alias Padwave.Output.Hid

  @frame 480
  @channels 2
  @report_size 398
  # The state snapshot: only the audio fields are claimed. This mirrors the
  # audio service's trimmed snapshot (see docs/protocol.md, "Two writers").
  @state_size 63

  @doc """
  Plays white noise for `duration_ms`, one 10 ms packet at a time.

  Options: `:level` (peak amplitude, default 3000), `:output` (`:jack` or
  `:speaker`, default `:jack` — which is where the headset in your setup is).
  """
  def noise(duration_ms, opts \\ [])

  def noise(duration_ms, opts) when is_integer(duration_ms) and duration_ms > 0 do
    level = Keyword.get(opts, :level, 3_000) |> max(0) |> min(30_000)
    output = Keyword.get(opts, :output, :jack)
    packets = max(1, div(duration_ms, 10))

    # One packet synchronously: if the encoder or the report is wrong, the API
    # call says so instead of leaving an unsupervised task to die quietly.
    case send_one(level, output) do
      {:ok, _bytes} ->
        Task.start(fn -> play(packets, level, output) end)
        {:ok, "noise: #{packets * 10} ms, level #{level}, #{output}"}

      {:error, reason} ->
        {:error, reason}
    end
  end

  def noise(_duration_ms, _opts), do: {:error, :bad_duration}

  @doc """
  Streams a file to the controller through ffmpeg, our Opus encoder and our own
  `0x36` reports — no mpv, no PipeWire sink.

  This is a prototype for the idea of owning the playback path: ffmpeg only
  decodes, and everything after it is the code we already have. Pacing is a
  10 ms scheduler on a monotonic clock; the decoder is allowed to run ahead by
  whatever it likes, because the send loop is the clock.

  Stop the radio (and LinuxAudio4Dualsense5) first: two writers of `0x36` fight.
  """
  def play_file(path, opts \\ []) when is_binary(path) do
    limit = Keyword.get(opts, :limit_ms, 30_000)
    output = Keyword.get(opts, :output, :jack)

    Task.start(fn -> stream_file(path, limit, output) end)

    {:ok, "ffmpeg -> 0x36: #{Path.basename(path)} (#{limit} ms)"}
  end

  @doc "One complete `0x36` report carrying `opus` (padded to 200 bytes)."
  def build(opus, opts \\ []) when is_binary(opus) do
    seq = Keyword.get(opts, :seq, 0) |> Bitwise.band(0x0F)
    counter = Keyword.get(opts, :counter, seq) |> Bitwise.band(0xFF)
    output = Keyword.get(opts, :output, :jack)
    control = if output == :speaker, do: 0x09, else: 0x00
    tag = if output == :speaker, do: 0x13, else: 0x16

    body =
      <<0x36, seq <<< 4, 0x11 ||| 0x80, 7, 0xFE, 64, 64, 64, 64, 64, counter>> <>
        <<0x10 ||| 0x80, @state_size>> <>
        state(control) <>
        <<0x12 ||| 0x80, 64>> <>
        :binary.copy(<<0>>, 64) <>
        <<tag ||| 0x80, 200>> <>
        pad_opus(opus) <>
        :binary.copy(<<0>>, 50)

    crc = :erlang.crc32(<<0xA2>> <> body)
    body <> <<crc::little-32>>
  end

  @doc "True when `report` is a `0x36` of the right size with a good CRC."
  def valid?(report) when is_binary(report) and byte_size(report) == @report_size do
    <<body::binary-size(@report_size - 4), crc::little-32>> = report
    :erlang.crc32(<<0xA2>> <> body) == crc
  end

  def valid?(_report), do: false

  @doc "The 63-byte state snapshot, with `audio_control` set for the output."
  def state(control) when control in [0x00, 0x09] do
    zero = :binary.copy(<<0>>, @state_size)

    zero
    # valid_flag0: volumes + audio routing
    |> put(0x00, 0xF0)
    # valid_flag1: audio_control2
    |> put(0x01, 0x80)
    # headphone_volume
    |> put(0x04, 0x7F)
    # speaker_volume
    |> put(0x05, 0x64)
    |> put(0x07, control)
    # audio_control2, as the audio service sends it
    |> put(0x25, 0x0A)
  end

  defp send_one(level, output) do
    port = open_encoder()

    try do
      opus = encode(port, pcm_frame(level))
      packet = build(opus, output: output)
      Hid.send_audio(packet)
      {:ok, byte_size(packet)}
    rescue
      error -> {:error, Exception.message(error)}
    after
      if port, do: Port.close(port)
    end
  end

  defp stream_file(path, limit_ms, output) do
    ffmpeg = System.find_executable("ffmpeg") || raise "ffmpeg is not installed"
    encoder = open_encoder()

    port =
      Port.open({:spawn_executable, ffmpeg}, [
        :binary,
        :exit_status,
        :stderr_to_stdout,
        {:args, ["-v", "error", "-nostdin", "-i", path, "-f", "s16le", "-ar", "48000", "-ac", "2", "-"]}
      ])

    try do
      pump(port, encoder, <<>>, 0, now(), limit_ms, output)
    after
      if encoder, do: Port.close(encoder)
      if Port.info(port), do: Port.close(port)
    end
  end

  # Read decoded PCM, cut it into 480-sample frames, and send one frame every
  # 10 ms. The sleep is computed from the start time rather than accumulated, so
  # a slow decode does not make the stream drift.
  defp pump(port, encoder, buffer, sent, started, limit_ms, output) do
    cond do
      now() - started > limit_ms ->
        :ok

      true ->
        receive do
          {^port, {:data, chunk}} ->
            {frames, rest} = frames(buffer <> chunk)
            sent = Enum.reduce(frames, sent, &send_frame(encoder, &1, &2, started, output))
            pump(port, encoder, rest, sent, started, limit_ms, output)

          {^port, {:exit_status, _status}} ->
            :ok
        after
          2_000 -> :ok
        end
    end
  end

  defp send_frame(encoder, pcm, seq, started, output) do
    wait = started + seq * 10 - now()
    if wait > 0, do: Process.sleep(wait)

    opus = encode(encoder, pcm)
    Hid.send_audio(build(opus, seq: seq, counter: seq, output: output))
    seq + 1
  end

  defp frames(buffer) do
    size = @frame * @channels * 2
    do_frames(buffer, size, [])
  end

  defp do_frames(buffer, size, acc) when byte_size(buffer) >= size do
    <<frame::binary-size(size), rest::binary>> = buffer
    do_frames(rest, size, [frame | acc])
  end

  defp do_frames(buffer, _size, acc), do: {Enum.reverse(acc), buffer}

  defp play(packets, level, output) do
    port = open_encoder()

    try do
      Enum.each(1..packets, fn seq ->
        pcm = pcm_frame(level)
        opus = encode(port, pcm)
        Hid.send_audio(build(opus, seq: seq, counter: seq, output: output))
        Process.sleep(10)
      end)
    after
      if port, do: Port.close(port)
    end
  end

  defp open_encoder do
    path =
      System.find_executable("padwave_opusdec") ||
        Path.expand("~/.local/bin/padwave_opusdec")

    Port.open({:spawn_executable, path}, [:binary, :exit_status, packet: 4, args: ["-e"]])
  end

  defp encode(port, pcm) do
    true = Port.command(port, pcm)

    receive do
      {^port, {:data, opus}} -> opus
    after
      1_000 -> :binary.copy(<<0>>, 200)
    end
  end

  # White noise, both channels, 480 samples each, s16le.
  defp pcm_frame(level) do
    for _ <- 1..(@frame * @channels), into: <<>> do
      sample = :rand.uniform(2 * level + 1) - (level + 1)
      <<sample::little-signed-16>>
    end
  end

  defp pad_opus(opus) when byte_size(opus) >= 200, do: binary_part(opus, 0, 200)
  defp pad_opus(opus), do: opus <> :binary.copy(<<0>>, 200 - byte_size(opus))

  defp now, do: System.monotonic_time(:millisecond)

  defp put(bin, offset, byte) do
    <<head::binary-size(offset), _, tail::binary>> = bin
    <<head::binary, byte, tail::binary>>
  end
end
