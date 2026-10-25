defmodule Padwave.Web.Server do
  @moduledoc false

  require Logger

  alias Padwave.Config

  def child_spec(_opts) do
    %{id: __MODULE__, start: {__MODULE__, :start_link, []}, type: :worker}
  end

  def start_link do
    if Config.get(:web_enabled, true) do
      port = Config.get(:web_port, 8787)

      case Bandit.start_link(plug: Padwave.Web.Router, port: port, ip: {0, 0, 0, 0}) do
        {:ok, pid} ->
          Logger.info("[web] listening on http://0.0.0.0:#{port}")
          {:ok, pid}

        {:error, reason} ->
          Logger.error("[web] cannot listen on #{port}: #{inspect(reason)}")
          :ignore
      end
    else
      :ignore
    end
  end
end
