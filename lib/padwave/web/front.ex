defmodule Padwave.Web.Front do
  @moduledoc """
  Runs `padwave serve` as the public web front.

  The C++ server owns `front_port` (the URL people use), serves the panel from
  its binary and proxies `/api/*` to this app on `web_port`. Keeping it a child
  of the BEAM means the systemd unit stays one service; if the binary is
  missing or the port is taken the backend keeps running on `web_port` and the
  front retries.
  """
  use GenServer
  require Logger

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @impl true
  def init(_opts) do
    send(self(), :start)
    {:ok, %{port: nil}}
  end

  @impl true
  def handle_info(:start, state) do
    case find_binary() do
      nil ->
        Logger.warning("[front] padwave binary not found; the API is only on the backend port")
        Process.send_after(self(), :start, 5_000)
        {:noreply, state}

      path ->
        front = Padwave.Config.get(:front_port, 8787)
        backend = Padwave.Config.get(:web_port, 8788)
        internal = Padwave.Config.get(:internal_port, 8890)

        port =
          Port.open({:spawn_executable, path}, [
            :binary,
            :exit_status,
            {:line, 1024},
            {:args, ["serve", to_string(front), to_string(backend), to_string(internal)]}
          ])

        Logger.info("[front] padwave serve #{front} -> 127.0.0.1:#{backend}")
        {:noreply, %{state | port: port}}
    end
  end

  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    Logger.warning("[front] exited (#{status}); retrying")
    Process.send_after(self(), :start, 2_000)
    {:noreply, %{state | port: nil}}
  end

  def handle_info({port, {:data, {:eol, _line}}}, %{port: port} = state), do: {:noreply, state}
  def handle_info(_other, state), do: {:noreply, state}

  defp find_binary do
    System.find_executable("padwave") ||
      case Path.expand("~/.local/bin/padwave") do
        path -> if File.exists?(path), do: path, else: nil
      end
  end
end
