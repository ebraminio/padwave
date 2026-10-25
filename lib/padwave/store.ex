defmodule Padwave.Store do
  @moduledoc """
  Debounced JSON persistence for the resume point and volume.

  Writes are coalesced so that a stream of trigger presses does not hammer the
  SD card. The file is replaced atomically (write + rename).
  """

  use GenServer
  require Logger

  alias Padwave.Config

  @flush_after_ms 1500

  def start_link(_opts) do
    GenServer.start_link(__MODULE__, nil, name: __MODULE__)
  end

  @doc "The persisted map, as loaded from disk."
  def load, do: GenServer.call(__MODULE__, :load)

  @doc "Merges `data` into the persisted map and schedules a flush."
  def save(data) when is_map(data), do: GenServer.cast(__MODULE__, {:save, data})

  @impl true
  def init(_opts) do
    path = Config.state_file()
    data = read(path)
    {:ok, %{path: path, data: data, timer: nil}}
  end

  @impl true
  def handle_call(:load, _from, state), do: {:reply, state.data, state}

  @impl true
  def handle_cast({:save, data}, state) do
    state = %{state | data: Map.merge(state.data, data)}
    {:noreply, schedule_flush(state)}
  end

  @impl true
  def handle_info(:flush, state) do
    write(state.path, state.data)
    {:noreply, %{state | timer: nil}}
  end

  def handle_info(_other, state), do: {:noreply, state}

  defp schedule_flush(%{timer: nil} = state) do
    %{state | timer: Process.send_after(self(), :flush, @flush_after_ms)}
  end

  defp schedule_flush(state), do: state

  defp read(path) do
    with {:ok, bin} <- File.read(path),
         {:ok, map} when is_map(map) <- Jason.decode(bin) do
      map
    else
      _ -> %{}
    end
  end

  defp write(path, data) do
    tmp = path <> ".tmp"
    File.mkdir_p!(Path.dirname(path))
    File.write!(tmp, Jason.encode!(data, pretty: true))
    File.rename!(tmp, path)
  rescue
    error ->
      Logger.warning("[store] could not persist state: #{Exception.message(error)}")
  end
end
