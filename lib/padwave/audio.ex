defmodule Padwave.Audio do
  @moduledoc """
  Watches the PipeWire side of things.

  padwave deliberately does *not* speak the speaker protocol itself: the
  controller's Bluetooth audio path is Opus-in-HID-report `0x36`, paced at
  512/48000 s, with its own CRC and a config mask that took a lot of
  reverse-engineering. LinuxAudio4Dualsense5 already implements it as a normal
  PipeWire sink, which is exactly the right boundary: we push PCM into a sink,
  it owns the radio link.

  What we do need to know is whether that sink is still alive and whether our
  players are on it — so this process polls `pactl` every few seconds and
  caches the answer for the UI.
  """

  use GenServer
  require Logger

  alias Padwave.Config

  @poll_ms 5_000

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  def status, do: GenServer.call(__MODULE__, :status, 2_000)

  @impl true
  def init(_opts) do
    send(self(), :poll)

    {:ok, %{sink: Config.get(:audio_sink, "ds5_membrane"), present: false, state: nil, streams: []}}
  end

  @impl true
  def handle_call(:status, _from, state), do: {:reply, state, state}

  @impl true
  def handle_info(:poll, state) do
    Process.send_after(self(), :poll, @poll_ms)
    {:noreply, probe(state.sink)}
  end

  def handle_info(_other, state), do: {:noreply, state}

  defp probe(sink_name) do
    case System.cmd("pactl", ["list", "short", "sinks"], stderr_to_stdout: true) do
      {output, 0} ->
        sink =
          output
          |> String.split("\n", trim: true)
          |> Enum.map(&String.split(&1, "\t"))
          |> Enum.find(fn columns -> Enum.at(columns, 1) == sink_name end)

        index = sink && Enum.at(sink, 0) |> to_int()

        %{
          sink: sink_name,
          present: sink != nil,
          state: sink && Enum.at(sink, 4),
          streams: if(index, do: input_streams(index), else: [])
        }

      {error, _code} ->
        Logger.debug("[audio] pactl failed: #{String.trim(error)}")
        %{sink: sink_name, present: false, state: nil, streams: []}
    end
  end

  defp input_streams(sink_index) do
    case System.cmd("pactl", ["list", "sink-inputs"], stderr_to_stdout: true) do
      {output, 0} ->
        output
        |> String.split("\n\n", trim: true)
        |> Enum.filter(fn block -> String.contains?(block, "Sink: #{sink_index}") end)
        |> Enum.map(fn block ->
          case Regex.run(~r/application\.name = "([^"]+)"/, block) do
            [_, name] -> name
            _ -> "unknown"
          end
        end)

      _ ->
        []
    end
  end

  defp to_int(nil), do: nil

  defp to_int(string) do
    case Integer.parse(string) do
      {value, _} -> value
      :error -> nil
    end
  end
end
