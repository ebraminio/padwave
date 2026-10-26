defmodule Padwave.Application do
  @moduledoc false

  use Application
  require Logger

  # A player whose BEAM died without running terminate/2 keeps its mpv and its
  # PipeWire stream alive. There can only be one padwave, so clear the socket
  # names we own before starting ours.
  defp sweep_stale_players do
    if Padwave.Config.get(:sweep_stale_players, true) do
      System.cmd("pkill", ["-f", "input-ipc-server=/tmp/padwave-"], stderr_to_stdout: true)
    end
  end

  @impl true
  def start(_type, _args) do
    Padwave.Config.ensure_dirs!()
    Padwave.Config.load_overrides!()
    sweep_stale_players()

    children = [
      {Padwave.Bus, []},
      Padwave.Store,
      Padwave.Audio,
      Padwave.Output.Hid,
      Padwave.Output.Lightbar,
      Padwave.Output.Triggers,
      Padwave.Output.ChargingLeds,
      Padwave.Player,
      Padwave.Radio,
      Padwave.Session,
      Padwave.Input.Mapper,
      Padwave.Input.Device,
      Padwave.Web.Server,
      Padwave.HotSwap
    ]

    opts = [strategy: :one_for_one, name: Padwave.Supervisor, max_restarts: 10, max_seconds: 30]

    case Supervisor.start_link(children, opts) do
      {:ok, pid} ->
        Logger.info("padwave up — web UI on http://0.0.0.0:#{Padwave.Config.get(:web_port)}")
        {:ok, pid}

      error ->
        error
    end
  end
end
