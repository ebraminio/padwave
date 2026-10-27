defmodule Padwave.Player.Native do
  @moduledoc """
  A playback engine that hands the whole audio path to `padwave native`.

  That program is one C file: libavformat/libavcodec decode → 48 kHz stereo →
  512-frame blocks → resample to the 480-sample Opus frame → libopus CBR →
  libopus → a complete `0x36` written straight to hidraw. Nothing crosses a
  process boundary per packet: this module only sends five word-commands
  (`play`, `pause`, `volume`, `seek`, `quit`) and reads the status lines the
  program prints ten times a second.

  The report bytes come from `scripts/padwave_schemas.hh`, so the protocol has
  one description (in C++) and this module only speaks the command/status line
  protocol to it.

  The port protocol deliberately stays line-oriented: it is debuggable from a
  shell without the app, which is how the program was tested.
  """

  use GenServer

  require Logger

  @binary "padwave"
  @status_broadcast_ms 500

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  def play(path, opts \\ []), do: GenServer.call(__MODULE__, {:play, path, opts}, 10_000)
  def pause(paused?), do: GenServer.cast(__MODULE__, {:pause, paused?})
  def set_volume(value) when is_number(value), do: GenServer.cast(__MODULE__, {:volume, value})
  def seek(seconds) when is_number(seconds), do: GenServer.cast(__MODULE__, {:seek, seconds})

  @doc "Seeks with a crossfade: the old position fades into the new one."
  def crossfade_seek(seconds, opts \\ []) when is_number(seconds),
    do: GenServer.cast(__MODULE__, {:crossfade_seek, seconds, Keyword.get(opts, :fade, 600)})
  def stop(opts \\ []), do: GenServer.cast(__MODULE__, {:stop, opts})

  @doc """
  Sends a line to the player process on its stdin. This is how the output
  writer lives in the same server: `fields ...` builds a 0x31 report, `raw
  <hex>` writes an already-built one.
  """
  def command(line) when is_binary(line), do: GenServer.cast(__MODULE__, {:command, line})
  @doc "Appends paths to the player's gapless queue."
  def enqueue(paths) when is_list(paths), do: GenServer.cast(__MODULE__, {:enqueue, paths})

  def position, do: GenServer.call(__MODULE__, :position, 2_000)
  def duration, do: GenServer.call(__MODULE__, :duration, 2_000)
  def state, do: GenServer.call(__MODULE__, :state, 2_000)

  @impl true
  def init(_opts) do
    Process.flag(:priority, :high)
    {:ok, new_state()}
  end

  @impl true
  def handle_call({:play, path, opts}, _from, state) do
    start = Keyword.get(opts, :start) || 0

    case ensure_port(state) do
      {:ok, state} ->
        state = %{
          state
          | path: path,
            position: start * 1.0,
            duration: nil,
            paused: false,
            eof: false,
            volume: state.volume
        }

        send_line(state, "fade #{round(Keyword.get(opts, :fade, 0) || 0)}\n")
        send_line(state, play_line(path, start))

        for queued <- Keyword.get(opts, :queue, []) do
          send_line(state, "queue #{queued}\n")
        end

        Process.send_after(self(), :status_tick, @status_broadcast_ms)
        {:reply, :ok, state}

      {:error, state} ->
        {:reply, :ok, state}
    end
  end

  def handle_call(:position, _from, state), do: {:reply, state.position, state}
  def handle_call(:duration, _from, state), do: {:reply, state.duration, state}

  def handle_call(:state, _from, state) do
    {:reply,
     %{
       engine: :native,
       path: state.path,
       position: Float.round(state.position * 1.0, 2),
       duration: state.duration,
       paused: state.paused,
       volume: round(state.volume),
       sent: state.sent,
       underruns: state.underruns,
       port: state.port != nil
     }, state}
  end

  @impl true
  def handle_cast({:pause, paused?}, state) do
    send_line(state, "pause #{if paused?, do: 1, else: 0}\n")
    {:noreply, %{state | paused: paused?}}
  end

  def handle_cast({:volume, value}, state) do
    send_line(state, "volume #{round(value)}\n")
    {:noreply, %{state | volume: value}}
  end

  def handle_cast({:crossfade_seek, seconds, fade}, state) do
    send_line(state, "seek #{round(seconds * 1000)} #{round(fade)}\n")
    {:noreply, %{state | position: seconds * 1.0, eof: false}}
  end

  def handle_cast({:seek, seconds}, state) do
    send_line(state, "seek #{round(seconds * 1000)}\n")
    {:noreply, %{state | position: seconds * 1.0}}
  end

  def handle_cast({:enqueue, paths}, state) do
    Enum.each(paths, fn path -> send_line(state, "queue #{path}\n") end)
    {:noreply, state}
  end

  def handle_cast({:command, line}, state) do
    case ensure_port(state) do
      {:ok, state} ->
        send_line(state, line <> "\n")
        {:noreply, state}

      {:error, state} ->
        {:noreply, %{state | errors: state.errors + 1}}
    end
  end

  def handle_cast({:stop, _opts}, state) do
    send_line(state, "quit\n")
    if state.port, do: Port.close(state.port)
    {:noreply, %{new_state() | volume: state.volume}}
  end

  @impl true
  def handle_info({port, {:data, {:eol, line}}}, %{port: port} = state) do
    {:noreply, parse_line(String.trim(line), state)}
  end

  def handle_info({port, {:data, {:noeol, _chunk}}}, %{port: port} = state), do: {:noreply, state}

  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    if status != 0, do: Logger.warning("[native engine] padwave native exited with #{status}")
    {:noreply, %{new_state() | volume: state.volume}}
  end

  def handle_info(:status_tick, state) do
    if state.port && state.path do
      Padwave.Bus.broadcast({:player, {:position, state.position}})
      if state.duration, do: Padwave.Bus.broadcast({:player, {:duration, state.duration}})
      Process.send_after(self(), :status_tick, @status_broadcast_ms)
    end

    {:noreply, state}
  end

  def handle_info(_other, state), do: {:noreply, state}

  @impl true
  def terminate(_reason, state) do
    if state.port, do: Port.close(state.port)
    :ok
  end

  # -- port plumbing ----------------------------------------------------------

  defp new_state do
    %{
      port: nil,
      path: nil,
      position: 0.0,
      duration: nil,
      paused: false,
      volume: 80,
      sent: 0,
      underruns: 0,
      errors: 0,
      eof: false
    }
  end

  defp ensure_port(%{port: nil} = state) do
    case find_binary() do
      nil ->
        Logger.warning("[native engine] #{@binary} not installed")
        {:error, state}

      path ->
        port =
          Port.open({:spawn_executable, path}, [
            :binary,
            :exit_status,
            {:line, 1024},
            {:args, ["native"]}
          ])

        {:ok, %{state | port: port}}
    end
  rescue
    error ->
      Logger.warning("[native engine] cannot start #{@binary} native: #{Exception.message(error)}")
      {:error, state}
  end

  defp ensure_port(state), do: {:ok, state}

  defp find_binary do
    System.find_executable(@binary) ||
      case Path.expand("~/.local/bin/#{@binary}") do
        path -> if File.exists?(path), do: path, else: nil
      end
  end

  # Commands go through the web front's loopback-only listener so the front can
  # own the player path next; the port's stdin stays the fallback for the
  # startup window before the front is listening.
  defp send_line(%{port: nil}, _line), do: :ok

  defp send_line(%{port: port}, line) do
    case front_command(line) do
      :ok -> :ok
      :error -> Port.command(port, line)
    end
  end

  defp front_command(line) do
    internal = Padwave.Config.get(:internal_port, 8890)
    url = ~c"http://127.0.0.1:#{internal}/internal/player"

    case :httpc.request(
           :post,
           {url, [], ~c"application/octet-stream", line},
           [timeout: 800, connect_timeout: 300],
           []
         ) do
      {:ok, {{_, 200, _}, _, _}} -> :ok
      _ -> :error
    end
  rescue
    _ -> :error
  end

  # The start time comes first: the path may contain spaces, and the player's
  # parser must not have to guess where one ends.
  defp play_line(path, start), do: "play #{round(start * 1000)} #{path}\n"

  # -- status -----------------------------------------------------------------

  defp parse_line("err" <> _, state), do: %{state | errors: state.errors + 1}
  defp parse_line("ok" <> _, state), do: state

  defp parse_line("pos " <> value, state) do
    case Integer.parse(value) do
      {ms, _} -> %{state | position: ms / 1000}
      _ -> state
    end
  end

  defp parse_line("dur " <> value, state) do
    case Integer.parse(value) do
      {0, _} -> state
      {ms, _} -> %{state | duration: ms / 1000}
      _ -> state
    end
  end

  defp parse_line("sent " <> value, state) do
    case Integer.parse(value) do
      {n, _} -> %{state | sent: n}
      _ -> state
    end
  end

  defp parse_line("underruns " <> value, state) do
    case Integer.parse(value) do
      {n, _} -> %{state | underruns: n}
      _ -> state
    end
  end

  defp parse_line("track " <> path, state) do
    Padwave.Bus.broadcast({:player, {:track, path}})
    %{state | path: path, position: 0.0, duration: nil, eof: false}
  end

  defp parse_line("eof", %{eof: false} = state) do
    Padwave.Bus.broadcast({:player, :eof})
    %{state | eof: true}
  end

  defp parse_line(_other, state), do: state
end
