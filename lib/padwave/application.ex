defmodule Padwave.Application do
  @moduledoc false

  use Application
  require Logger

  @impl true
  def start(_type, _args) do
    Padwave.Config.ensure_dirs!()

    children = [
      {Padwave.Bus, []},
      Padwave.Store,
      Padwave.Output.Lightbar,
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
