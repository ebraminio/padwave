defmodule Padwave.Player.Slot do
  @moduledoc """
  One `mpv` instance, controlled over its JSON IPC socket.

  Two slots are kept alive permanently so that a station change is a crossfade
  between two already-running players rather than a stop/start click. mpv is
  pinned to the DualSense sink, which also means a system default-sink change
  cannot steal the music.
  """

  use GenServer
  require Logger

  alias Padwave.Config

  def start_link(opts), do: GenServer.start_link(__MODULE__, opts)

  def load(pid, path, opts \\ []), do: GenServer.cast(pid, {:load, path, opts})
  def volume(pid, value), do: GenServer.cast(pid, {:volume, value})
  def pause(pid, paused?), do: GenServer.cast(pid, {:pause, paused?})
  def stop(pid), do: GenServer.cast(pid, :stop)

  def seek(pid, seconds) when is_number(seconds),
    do: GenServer.call(pid, {:seek, seconds}, 5000)

  def quit(pid), do: GenServer.cast(pid, :quit)

  @impl true
  def init(opts) do
    name = Keyword.fetch!(opts, :name)
    owner = Keyword.fetch!(opts, :owner)
    sock = Path.join(System.tmp_dir!(), "padwave-#{name}-#{System.pid()}.sock")
    _ = File.rm(sock)
    Process.flag(:trap_exit, true)

    exe =
      System.find_executable("mpv") ||
        raise "mpv is not installed (sudo apt install mpv)"

    port =
      Port.open({:spawn_executable, String.to_charlist(exe)}, [
        :binary,
        :exit_status,
        :stderr_to_stdout,
        {:line, 2048},
        {:args, mpv_args(name, sock)}
      ])

    {:ok,
     %{
       name: name,
       owner: owner,
       sock: sock,
       port: port,
       conn: nil,
       tries: 0,
       req: 0,
       pending: %{},
       queue: [],
       loaded: nil,
       paused: false,
       volume: 0
     }, {:continue, :connect}}
  end

  @impl true
  def handle_continue(:connect, state) do
    send(self(), :connect)
    {:noreply, state}
  end

  @impl true
  def handle_call({:seek, seconds}, from, state) do
    call(state, ["seek", seconds, "absolute"], from)
  end

  @impl true
  def handle_cast({:load, path, opts}, state) do
    start = Keyword.get(opts, :start) || 0

    command =
      if is_number(start) and start > 0.5 do
        ["loadfile", path, "replace", "start=" <> Float.to_string(Float.round(start * 1.0, 3))]
      else
        ["loadfile", path, "replace"]
      end

    state = enqueue(state, command)
    {:noreply, %{state | loaded: path}}
  end

  def handle_cast({:volume, value}, state) do
    command = ["set_property", "volume", value]
    {:noreply, %{state | volume: value} |> enqueue(command)}
  end

  def handle_cast({:pause, paused?}, state) do
    {:noreply, %{state | paused: paused?} |> enqueue(["set_property", "pause", paused?])}
  end

  def handle_cast(:stop, state) do
    {:noreply, %{state | loaded: nil} |> enqueue(["stop"])}
  end

  def handle_cast(:quit, state) do
    enqueue(state, ["quit"])
    {:noreply, state}
  end

  @impl true
  def handle_info(:connect, %{conn: nil} = state) do
    case :gen_tcp.connect({:local, state.sock}, 0, [:binary, packet: :line, active: true], 500) do
      {:ok, conn} ->
        Logger.info("[slot #{state.name}] ipc connected")
        state = %{state | conn: conn, tries: 0}
        Process.send_after(self(), :poll, 400)
        {:noreply, flush_queue(state)}

      {:error, _reason} when state.tries < 200 ->
        Process.send_after(self(), :connect, 50)
        {:noreply, %{state | tries: state.tries + 1}}

      {:error, reason} ->
        Logger.error("[slot #{state.name}] mpv ipc connect failed: #{inspect(reason)}")
        {:noreply, state}
    end
  end

  def handle_info(:connect, state), do: {:noreply, state}

  def handle_info({:tcp, conn, line}, %{conn: conn} = state) do
    state =
      case Jason.decode(String.trim(line)) do
        {:ok, %{"request_id" => id} = message} -> complete(state, id, message)
        {:ok, %{"event" => _} = event} -> handle_event(state, event)
        _other -> state
      end

    {:noreply, state}
  end

  def handle_info({:tcp_closed, conn}, %{conn: conn} = state) do
    Logger.warning("[slot #{state.name}] mpv ipc closed")
    {:noreply, %{state | conn: nil}}
  end

  def handle_info(:poll, state) do
    Process.send_after(self(), :poll, 500)
    {:noreply, if(state.loaded, do: poll(state, "time-pos", :position), else: state)}
  end

  def handle_info({:req_timeout, id}, state) do
    case Map.pop(state.pending, id) do
      {{:call, from}, pending} ->
        GenServer.reply(from, {:error, :timeout})
        {:noreply, %{state | pending: pending}}

      _ ->
        {:noreply, state}
    end
  end

  def handle_info({port, {:data, {:eol, line}}}, %{port: port} = state) do
    Logger.debug("[slot #{state.name}] #{line}")
    {:noreply, state}
  end

  def handle_info({port, {:data, {:noeol, chunk}}}, %{port: port} = state) do
    Logger.debug("[slot #{state.name}] #{chunk}")
    {:noreply, state}
  end

  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    Logger.warning("[slot #{state.name}] mpv exited with #{status}")
    {:noreply, %{state | conn: nil}}
  end

  def handle_info({:EXIT, port, reason}, %{port: port} = state) do
    Logger.warning("[slot #{state.name}] mpv port died: #{inspect(reason)}")
    {:noreply, %{state | conn: nil}}
  end

  def handle_info(_other, state), do: {:noreply, state}

  @impl true
  def terminate(_reason, state) do
    if state.conn, do: :gen_tcp.close(state.conn)
    if state.port, do: Port.close(state.port)
    :ok
  rescue
    _ -> :ok
  end

  # -- mpv protocol ----------------------------------------------------------

  defp mpv_args(name, sock) do
    [
      "--no-config",
      "--idle=yes",
      "--no-video",
      "--vo=null",
      "--ao=pipewire",
      "--audio-client-name=padwave-" <> Atom.to_string(name),
      "--volume=0",
      "--volume-max=100",
      "--terminal=yes",
      "--term-status-msg=",
      "--msg-level=all=error",
      "--input-ipc-server=" <> sock,
      "--keep-open=no",
      "--gapless-audio=no",
      "--prefetch-playlist=no",
      "--cache=no",
      "--no-osc",
      "--no-input-default-bindings",
      "--no-input-terminal"
    ] ++ audio_device_args()
  end

  defp audio_device_args do
    case Config.get(:audio_device) do
      device when is_binary(device) and device != "" -> ["--audio-device=" <> device]
      _ -> []
    end
  end

  defp enqueue(%{conn: nil} = state, command) do
    %{state | queue: state.queue ++ [command]}
  end

  defp enqueue(state, command) do
    send_json(state, %{"command" => command})
    state
  end

  defp flush_queue(%{conn: nil} = state), do: state

  defp flush_queue(state) do
    Enum.each(state.queue, fn command ->
      send_json(state, %{"command" => command})
    end)

    %{state | queue: []}
  end

  defp call(%{conn: nil} = state, _command, from) do
    GenServer.reply(from, {:error, :not_connected})
    {:noreply, state}
  end

  defp call(state, command, from) do
    id = state.req + 1
    timer = Process.send_after(self(), {:req_timeout, id}, 4000)

    case send_json(state, %{"command" => command, "request_id" => id}) do
      :ok ->
        {:noreply, %{state | req: id, pending: Map.put(state.pending, id, {:call, from, timer})}}

      {:error, reason} ->
        Process.cancel_timer(timer)
        GenServer.reply(from, {:error, reason})
        {:noreply, state}
    end
  end

  defp poll(state, property, tag) do
    id = state.req + 1

    case send_json(state, %{"command" => ["get_property", property], "request_id" => id}) do
      :ok -> %{state | req: id, pending: Map.put(state.pending, id, {:poll, tag})}
      {:error, _reason} -> state
    end
  end

  defp send_json(%{conn: nil}, _payload), do: {:error, :not_connected}

  defp send_json(%{conn: conn}, payload) do
    :gen_tcp.send(conn, [Jason.encode!(payload), "\n"])
  end

  defp complete(state, id, %{"error" => error} = message) do
    case Map.pop(state.pending, id) do
      {nil, _pending} ->
        state

      {{:call, from, timer}, pending} ->
        Process.cancel_timer(timer)

        result =
          case error do
            "success" -> {:ok, message["data"]}
            other -> {:error, other}
          end

        GenServer.reply(from, result)
        %{state | pending: pending}

      {{:poll, tag}, pending} ->
        state = %{state | pending: pending}

        case message["data"] do
          value when is_number(value) -> notify_owner(state, {:poll, tag, value * 1.0})
          _ -> state
        end
    end
  end

  defp complete(state, _id, _message), do: state

  defp handle_event(state, %{"event" => "file-loaded"}) do
    state = poll(state, "duration", :duration)
    notify_owner(state, {:loaded, state.loaded})
  end

  defp handle_event(state, %{"event" => "end-file"} = event) do
    notify_owner(state, {:end_file, event["reason"] || "unknown"})
  end

  defp handle_event(state, %{"event" => "playback-restart"}) do
    state = poll(state, "duration", :duration)
    notify_owner(state, :playing)
  end

  defp handle_event(state, _event), do: state

  defp notify_owner(state, message) do
    send(state.owner, {:slot, state.name, message})
    state
  end
end
