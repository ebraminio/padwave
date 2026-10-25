defmodule Padwave.Web.Router do
  @moduledoc """
  A small JSON API, a server-sent-events telemetry stream, and the single-page
  UI.

  The UI holds one long-lived `GET /api/events` connection and paints from it;
  there is no polling, so button presses and the playhead arrive at 5 Hz and
  the progress bar is interpolated client-side between frames.
  """

  use Plug.Router

  alias Padwave.{Config, Input, Radio, Session}

  @event_interval_ms 200

  plug(:match)
  plug(Plug.Logger, log: :debug)
  plug(Plug.Parsers, parsers: [:urlencoded, :json], pass: ["*/*"], json_decoder: Jason)
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
    json(conn, status_payload())
  end

  get "/api/events" do
    conn =
      conn
      |> put_resp_header("content-type", "text/event-stream")
      |> put_resp_header("cache-control", "no-cache, no-transform")
      |> put_resp_header("x-accel-buffering", "no")
      |> send_chunked(200)

    stream(conn, 0)
  end

  get "/api/library" do
    json(conn, %{tracks: Radio.tracks()})
  end

  get "/api/schema" do
    json(conn, %{
      input: Padwave.Schema.DualSenseInput.spec(:input),
      output: Padwave.Schema.DualSenseOutput.spec(:output),
      bindings: Input.Mapper.bindings()
    })
  end

  get "/api/config" do
    json(conn, %{ok: true, config: Config.public(), editable: Config.runtime_keys()})
  end

  # Accepts JSON, form encoding, or query parameters — `curl -d key=value`
  # without a content-type used to fail here, which is a bad first experience.
  post "/api/config" do
    update_config(conn, Map.merge(conn.params, conn.body_params))
  end

  # Alias kept for the curl one-liner that used to live in the README.
  post "/api/debug/config" do
    update_config(conn, Map.merge(conn.params, conn.body_params))
  end

  post "/api/command" do
    params = Map.merge(conn.params, conn.body_params)
    action = params["action"]
    args = Map.drop(params, ["action"])

    case Radio.command(action, args) do
      :ok -> json(conn, %{ok: true, status: Radio.status()})
      {:error, reason} -> json(conn, %{ok: false, error: inspect(reason), status: Radio.status()})
    end
  end

  post "/api/reload" do
    case safe_reload() do
      {:ok, modules} -> json(conn, %{ok: true, reloaded: Enum.map(modules, &inspect/1)})
      {:error, reason} -> json(conn, %{ok: false, error: to_string(reason)})
    end
  end

  post "/api/debug/frame" do
    patch = Map.merge(conn.params, conn.body_params)
    Padwave.Input.Simulator.press(patch, to_int(Map.get(patch, "hold_ms"), 140))
    json(conn, %{ok: true, patch: patch})
  end

  get "/api/debug" do
    radio = :sys.get_state(Padwave.Radio)

    json(conn, %{
      bus_members: :pg.get_members(Padwave.Bus, :events) |> Enum.map(&inspect/1),
      radio: %{
        position: radio.position,
        playing: radio.playing,
        paused: radio.paused,
        suspended: radio.suspended,
        current: radio.current && radio.current.id,
        volume: radio.volume
      },
      player: Padwave.Player.state(),
      audio: Padwave.Audio.status()
    })
  end

  get "/api/health" do
    json(conn, %{ok: true})
  end

  match _ do
    send_resp(conn, 404, "not found")
  end

  # -- the live stream -------------------------------------------------------

  defp stream(conn, sequence) do
    payload = Jason.encode!(status_payload())

    case chunk(conn, "id: #{sequence}\ndata: #{payload}\n\n") do
      {:ok, conn} ->
        Process.sleep(@event_interval_ms)
        stream(conn, sequence + 1)

      {:error, _closed} ->
        conn
    end
  end

  defp update_config(conn, params) do
    payload =
      case {params["key"], params["value"]} do
        {key, value} when is_binary(key) -> %{key => value}
        _ -> Map.take(params, Config.runtime_keys())
      end

    case Config.update(payload) do
      {:ok, config} -> json(conn, %{ok: true, config: config})
      {:error, reason} -> json(conn, %{ok: false, error: reason})
    end
  end

  defp safe_reload do
    Padwave.HotSwap.reload()
  rescue
    error -> {:error, Exception.message(error)}
  end

  # -- payloads --------------------------------------------------------------

  defp status_payload do
    %{
      radio: Radio.status(),
      session: Session.status(),
      audio: Padwave.Audio.status(),
      controller: controller(),
      bindings: Input.Mapper.bindings(),
      config: Config.public(),
      now: System.system_time(:second)
    }
  end

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
      temperature: frame && frame.temperature,
      accel: frame && [frame.fields.accel_x, frame.fields.accel_y, frame.fields.accel_z],
      gyro: frame && [frame.fields.gyro_x, frame.fields.gyro_y, frame.fields.gyro_z],
      raw: frame && Base.encode16(frame.raw, case: :lower),
      fields: frame && sanitize(frame.fields),
      last_action: snapshot.last_action && inspect(snapshot.last_action)
    }
  end

  defp sanitize(%{} = map), do: Map.new(map, fn {key, value} -> {key, sanitize(value)} end)
  defp sanitize(list) when is_list(list), do: Enum.map(list, &sanitize/1)
  defp sanitize(value) when is_binary(value), do: Base.encode16(value, case: :lower)
  defp sanitize(value) when is_atom(value) and not is_boolean(value), do: Atom.to_string(value)
  defp sanitize(value), do: value

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
end
