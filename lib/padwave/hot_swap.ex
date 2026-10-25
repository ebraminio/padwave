defmodule Padwave.HotSwap do
  @moduledoc """
  Reloads modules into the running service.

  The whole point of padwave running on the BEAM is that you can edit a packet
  description (or a button mapping, or the UI) and have it take effect in the
  *next* controller report, with the music never stopping.

  Changed files under `lib/` and `priv/` are detected by mtime and compiled in
  place. Beware that a *state shape* change in a GenServer will survive the
  swap only if the new code can still read the old state — the packet and
  mapping modules are the ones you want to hot-swap, and they are stateless.
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config}

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc "Compiles every source file that changed since the last scan."
  def reload, do: GenServer.call(__MODULE__, :reload, 60_000)

  @doc "Forces one file to be compiled and swapped."
  def reload(path), do: GenServer.call(__MODULE__, {:reload, path}, 60_000)

  @impl true
  def init(_opts) do
    enabled = Config.get(:hot_swap, true)

    if enabled do
      Process.send_after(self(), :scan, Config.get(:hot_swap_interval_ms, 1000))
    end

    {:ok, %{enabled: enabled, mtimes: snapshot()}}
  end

  @impl true
  def handle_call(:reload, _from, state) do
    {state, modules} = compile_changed(state)
    {:reply, {:ok, modules}, state}
  end

  def handle_call({:reload, path}, _from, state) do
    modules = compile(path)
    {:reply, {:ok, modules}, %{state | mtimes: Map.put(state.mtimes, path, mtime(path))}}
  end

  @impl true
  def handle_info(:scan, %{enabled: true} = state) do
    Process.send_after(self(), :scan, Config.get(:hot_swap_interval_ms, 1000))
    {state, _modules} = compile_changed(state)
    {:noreply, state}
  end

  def handle_info(:scan, state), do: {:noreply, state}
  def handle_info(_other, state), do: {:noreply, state}

  defp compile_changed(state) do
    {changed, mtimes} =
      Enum.reduce(snapshot(), {[], state.mtimes}, fn {path, m}, {changed, known} ->
        if Map.get(known, path) == m do
          {changed, known}
        else
          {[path | changed], Map.put(known, path, m)}
        end
      end)

    modules =
      changed
      |> Enum.reverse()
      |> Enum.flat_map(fn path ->
        try do
          compile(path)
        rescue
          error ->
            Logger.error("[hotswap] #{path}: #{Exception.message(error)}")
            []
        end
      end)

    if modules != [] do
      Logger.info("[hotswap] reloaded #{Enum.map_join(modules, ", ", &inspect/1)}")
      Bus.broadcast({:system, {:reloaded, modules}})
    end

    {%{state | mtimes: mtimes}, modules}
  end

  defp compile(path) do
    Code.compiler_options(ignore_module_conflict: true)

    try do
      modules =
        path
        |> Code.compile_file()
        |> Enum.map(fn {module, _binary} -> module end)

      Enum.each(modules, &:code.purge/1)
      modules
    after
      Code.compiler_options(ignore_module_conflict: false)
    end
  end

  defp snapshot do
    (("lib/**/*.ex" |> Path.wildcard()) ++ ("priv/**/*.ex" |> Path.wildcard()))
    |> Map.new(fn path -> {path, mtime(path)} end)
  end

  defp mtime(path) do
    case File.stat(path, time: :posix) do
      {:ok, %{mtime: mtime}} -> mtime
      _ -> nil
    end
  end
end
