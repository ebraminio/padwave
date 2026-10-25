defmodule Padwave.Web.Router do
  @moduledoc """
  A small JSON API plus the single-page UI.

  There is no build step and no frontend framework: the page is one HTML file
  served straight from `priv/ui`, which also means you can edit it and hit
  reload — or hot-reload it with the rest of the app.
  """

  use Plug.Router

  alias Padwave.{Config, Input, Radio, Session}

  plug(:match)
  plug(Plug.Logger, log: :debug)
  plug(Plug.Parsers, parsers: [:json], pass: ["*/*"], json_decoder: Jason)
  plug(:dispatch)

  get "/" do
    path = Path.join(:code.priv_dir(:padwave), "ui/index.html")

    case File.read(path) do
      {:ok, html} ->
        conn
        |> put_resp_content_type("text/html")
        |> put_resp_header("cache-control", "no-store")
        |> send_resp(200, html)

      {:error, reason} ->
        send_resp(conn, 500, "UI not found at #{path}: #{inspect(reason)}")
    end
  end

  get "/api/status" do
    json(conn, %{
      radio: Radio.status(),
      session: Session.status(),
      audio: Padwave.Audio.status(),
      controller: controller(),
      bindings: Input.Mapper.bindings(),
      now: System.system_time(:second)
    })
  end

  get "/api/library" do
    json(conn, %{tracks: Radio.tracks()})
  end

  get "/api/schema" do
    json(conn, %{
      input: Padwave.Schema.DualSenseInput.spec(:input),
      bindings: Input.Mapper.bindings()
    })
  end

  get "/api/debug" do
    radio = :sys.get_state(Padwave.Radio)

    json(conn, %{
      bus_members: :pg.get_members(Padwave.Bus, :events) |> Enum.map(&inspect/1),
      pids: %{
        radio: inspect(Process.whereis(Padwave.Radio)),
        player: inspect(Process.whereis(Padwave.Player)),
        session: inspect(Process.whereis(Padwave.Session)),
        mapper: inspect(Process.whereis(Padwave.Input.Mapper)),
        lightbar: inspect(Process.whereis(Padwave.Output.Lightbar))
      },
      radio: %{
        position: radio.position,
        playing: radio.playing,
        paused: radio.paused,
        suspended: radio.suspended,
        current: radio.current && radio.current.id,
        volume: radio.volume
      },
      player: Padwave.Player.state(),
      self: inspect(self())
    })
  end

  post "/api/debug/config" do
    reply =
      try do
        key = conn.body_params["key"]
        value = conn.body_params["value"]

        if is_binary(key) do
          atom = String.to_existing_atom(key)
          current = Application.get_env(:padwave, atom)
          Application.put_env(:padwave, atom, coerce(current, value))
          %{ok: true, config: %{key => Application.get_env(:padwave, atom)}}
        else
          %{ok: false, error: "key required"}
        end
      rescue
        error -> %{ok: false, error: Exception.message(error)}
      end

    json(conn, reply)
  end

  post "/api/debug/frame" do
    patch = conn.body_params
    Padwave.Input.Simulator.press(patch, Map.get(patch, "hold_ms", 140) |> to_int(140))
    json(conn, %{ok: true, patch: patch})
  end

  get "/api/health" do
    json(conn, %{ok: true, node: Node.self()})
  end

  post "/api/command" do
    action = conn.body_params["action"] || conn.params["action"]
    args = Map.drop(conn.body_params, ["action"])

    reply =
      case Radio.command(action, args) do
        :ok -> %{ok: true, status: Radio.status()}
        {:error, reason} -> %{ok: false, error: inspect(reason), status: Radio.status()}
      end

    json(conn, reply)
  end

  post "/api/reload" do
    result =
      try do
        Padwave.HotSwap.reload()
      rescue
        error -> {:error, Exception.message(error)}
      end

    case result do
      {:ok, modules} -> json(conn, %{ok: true, reloaded: Enum.map(modules, &inspect/1)})
      {:error, reason} -> json(conn, %{ok: false, error: to_string(reason)})
    end
  end

  match _ do
    send_resp(conn, 404, "not found")
  end

  # -- helpers ---------------------------------------------------------------

  defp controller do
    info = Input.Device.info()
    snapshot = Input.Mapper.snapshot()
    frame = snapshot.frame

    %{
      connected: info != nil,
      mac: info && info.mac,
      node: info && info.node,
      name: info && info.name,
      battery: frame && frame.battery,
      charging: frame && frame.charging && Atom.to_string(frame.charging),
      jack: frame && frame.jack,
      mic: frame && frame.mic,
      dpad: frame && Atom.to_string(frame.dpad),
      buttons: frame && Enum.map(frame.buttons, &Atom.to_string/1),
      l2: frame && frame.l2,
      r2: frame && frame.r2,
      lx: frame && frame.lx,
      ly: frame && frame.ly,
      rx: frame && frame.rx,
      ry: frame && frame.ry,
      raw: frame && Base.encode16(frame.raw, case: :lower),
      fields: frame && sanitize(frame.fields),
      last_action: snapshot.last_action && inspect(snapshot.last_action)
    }
  end

  defp sanitize(%{} = map) do
    Map.new(map, fn {key, value} -> {key, sanitize(value)} end)
  end

  defp sanitize(list) when is_list(list), do: Enum.map(list, &sanitize/1)
  defp sanitize(value) when is_binary(value), do: Base.encode16(value, case: :lower)
  defp sanitize(value) when is_atom(value) and not is_boolean(value), do: Atom.to_string(value)
  defp sanitize(value), do: value

  # Keeps the runtime config endpoint honest: numbers stay numbers, booleans
  # stay booleans, everything else is a string.
  defp coerce(_current, value) when is_integer(value) or is_float(value) or is_boolean(value),
    do: value

  defp coerce(current, value) when is_binary(value) do
    cond do
      is_integer(current) -> to_int(value, current)
      is_boolean(current) -> value in ["true", "1", "yes"]
      true -> value
    end
  end

  defp coerce(_current, value), do: value

  defp to_int(value, _default) when is_integer(value), do: value

  defp to_int(value, default) when is_binary(value) do
    case Integer.parse(value) do
      {number, _} -> number
      :error -> default
    end
  end

  defp to_int(_value, default), do: default

  defp json(conn, data) do
    conn
    |> put_resp_content_type("application/json")
    |> put_resp_header("cache-control", "no-store")
    |> send_resp(200, Jason.encode!(data))
  end

  @doc false
  def config_summary do
    %{
      music_roots: Config.music_roots(),
      audio_device: Config.get(:audio_device),
      web_port: Config.get(:web_port)
    }
  end
end
