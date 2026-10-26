defmodule Padwave.Mic do
  @moduledoc """
  Decodes the microphone frames that `Padwave.Input.Device` hands over.

  The frames themselves are described in `Padwave.Schema.DualSenseMic`: Opus,
  48 kHz mono, 10 ms each, marked by the `0xD4` TOC at byte 3 of an `0x31`
  report. This module is the other half — it keeps a long-lived
  `padwave_opusdec` process (libopus through a port, the same library the audio
  service links) and turns each decoded frame into a level for the panel.

  Nothing here is required for playback. It exists so the microphone path can be
  watched from the UI: `stats/0` reports packets, RMS, peak and the last frame's
  sample count. An Opus decoder per packet is pointless CPU if nobody is
  looking, so decoding only happens while a caller has asked for stats recently
  — the panel polling is the switch.

  ## Enabling the stream is not this module's job

  The controller only sends these frames if something asked it to: bit 0 of the
  `0x36` audio enable mask (plus the unmute in `power_save_control`, plus
  `mic_volume`). That is the audio service's `0x36` stream — see
  `Padwave.Schema.DualSenseMic` for the three steps. With capture off this module
  simply sees no frames, which is also a useful thing to observe.
  """

  use GenServer

  require Logger

  @decoder "padwave_opusdec"
  @idle_ms 5_000

  def start_link(opts \\ []), do: GenServer.start_link(__MODULE__, opts, name: __MODULE__)

  @doc "Hands one Opus packet (already stripped of TOC and CRC) to the decoder."
  def ingest(payload) when is_binary(payload), do: GenServer.cast(__MODULE__, {:packet, payload})

  @doc "Packet count, level and the last decoded frame size."
  def stats, do: GenServer.call(__MODULE__, :stats, 2_000)

  @doc "Forgets the counters (the decoder itself stays open)."
  def reset, do: GenServer.cast(__MODULE__, :reset)

  @impl true
  def init(_opts) do
    {:ok,
     %{
       port: nil,
       path: find_decoder(),
       packets: 0,
       frames: 0,
       errors: 0,
       rms: 0,
       peak: 0,
       samples: 0,
       last_at: nil,
       last_seen: nil
     }}
  end

  @impl true
  def handle_cast({:packet, payload}, state) do
    state = %{state | packets: state.packets + 1, last_at: now(), last_seen: now()}

    case ensure_port(state) do
      {:ok, state, port} ->
        case Port.command(port, payload) do
          true -> {:noreply, state}
          false -> {:noreply, %{state | errors: state.errors + 1}}
        end

      {:error, state} ->
        {:noreply, %{state | errors: state.errors + 1}}
    end
  end

  def handle_cast(:reset, state) do
    {:noreply, %{state | packets: 0, frames: 0, errors: 0, rms: 0, peak: 0, samples: 0}}
  end

  @impl true
  def handle_info({port, {:data, pcm}}, %{port: port} = state) do
    {rms, peak} = measure(pcm)

    {:noreply,
     %{
       state
       | frames: state.frames + 1,
         rms: rms,
         peak: peak,
         samples: byte_size(pcm) |> div(2),
         last_at: now()
     }}
  end

  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    Logger.warning("[mic] decoder exited (#{status}); will respawn on the next packet")
    {:noreply, %{state | port: nil}}
  end

  def handle_info(_other, state), do: {:noreply, state}

  @impl true
  def handle_call(:stats, _from, state) do
    active? = state.last_seen != nil and now() - state.last_seen < @idle_ms

    {:reply,
     %{
       decoder: state.path,
       running: state.port != nil,
       active: active?,
       packets: state.packets,
       frames: state.frames,
       errors: state.errors,
       rms: state.rms,
       peak: state.peak,
       level: level(state.rms),
       samples: state.samples,
       last_at: state.last_at
     }, state}
  end

  @impl true
  def terminate(_reason, %{port: port}) do
    if port, do: Port.close(port)
    :ok
  end

  defp ensure_port(%{port: nil, path: nil} = state), do: {:error, state}

  defp ensure_port(%{port: nil, path: path} = state) do
    case Port.open({:spawn_executable, path}, [:binary, :exit_status, packet: 4, args: []]) do
      port -> {:ok, %{state | port: port}, port}
    end
  rescue
    error ->
      Logger.warning("[mic] cannot start #{@decoder}: #{Exception.message(error)}")
      {:error, state}
  end

  defp ensure_port(%{port: port} = state), do: {:ok, state, port}

  defp find_decoder do
    found = System.find_executable(@decoder)
    local = Path.expand("~/.local/bin/#{@decoder}")

    cond do
      found ->
        found

      File.exists?(local) ->
        local

      true ->
        Logger.info("[mic] #{@decoder} not installed; microphone decoding disabled")
        nil
    end
  end

  # s16le mono: RMS and the largest absolute sample, both 0..32767.
  defp measure(pcm) do
    samples = for <<s::little-signed-16 <- pcm>>, do: s

    case samples do
      [] ->
        {0, 0}

      list ->
        sum = Enum.reduce(list, 0, fn s, acc -> acc + s * s end)
        {round(:math.sqrt(sum / length(list))), list |> Enum.map(&abs/1) |> Enum.max()}
    end
  end

  # A 0..100 scale that is useful rather than linear: a quiet room sits near 0
  # and speech fills the bar. 32767/8 is roughly -18 dBFS.
  defp level(rms) when rms <= 0, do: 0
  defp level(rms), do: min(100, round(100 * :math.log10(1 + rms / 8) / 3.6))

  defp now, do: System.monotonic_time(:millisecond)
end
