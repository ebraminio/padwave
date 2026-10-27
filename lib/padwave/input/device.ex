defmodule Padwave.Input.Device do
  @moduledoc """
  Finds the DualSense's `hidraw` node, runs `padwave input <node>`, and consumes
  its line protocol: one `frame` line per state report and `mic <hex>` for the
  Opus frames. The parsing and CRC live in C++ (`padwave_schemas.hh`); this
  process only watches for the controller appearing and turning those lines
  into `Frame` values for the mapper and the panel.

  The node is opened read-only by the CLI. The reader keeps re-scanning so that
  a controller that is switched off and back on is picked up without a service
  restart.
  """

  use GenServer
  require Logger

  alias Padwave.Bus
  alias Padwave.Input.Frame

  @rescan_ms 1000
  @sony 0x054C
  # 0x0CE6 is the DualSense, 0x0DF2 the DualSense Edge.
  @dualsense 0x0CE6
  @dualsense_edge 0x0DF2
  @supported [@dualsense, @dualsense_edge]

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc "Metadata for the connected controller, or nil."
  def info, do: GenServer.call(__MODULE__, :info, 2000)

  @doc "Whether a controller is currently readable."
  def connected?, do: GenServer.call(__MODULE__, :connected?, 2000)

  @doc "Path of the hidraw node, e.g. `/dev/hidraw0`."
  def node, do: with(%{node: node} <- info(), do: node, else: (_ -> nil))

  @impl true
  def init(_opts) do
    send(self(), :scan)

    {:ok,
     %{
       info: nil,
       pending: nil,
       port: nil,
       dropped: 0,
       rate: 0,
       frames: 0,
       since: now()
     }}
  end

  @impl true
  def handle_call(:info, _from, state) do
    info =
      if state.info do
        state.info
        |> Map.put(:dropped, state.dropped)
        |> Map.put(:rate, state.rate)
      end

    {:reply, info, state}
  end

  def handle_call(:connected?, _from, state), do: {:reply, not is_nil(state.info), state}

  @impl true
  def handle_info(:scan, state) do
    Process.send_after(self(), :scan, @rescan_ms)

    case discover() do
      nil ->
        {:noreply, shutdown(state, :not_found)}

      %{node: node} = info ->
        if state.info && state.info.node == node && state.port do
          {:noreply, %{state | info: info}}
        else
          {:noreply, state |> shutdown(:switched) |> start_reader(info)}
        end
    end
  end

  def handle_info({port, {:data, {:eol, "ready" <> _}}}, %{port: port, pending: info} = state)
      when info != nil do
    Logger.info("[controller] #{info.node} is a #{info.name} (#{info.mac}), reading")
    Bus.broadcast({:device, :up, info})
    {:noreply, %{state | info: info, pending: nil}}
  end

  def handle_info({port, {:data, {:eol, "frame " <> _ = line}}}, %{port: port} = state) do
    case Frame.from_line(line) do
      nil -> {:noreply, state}
      frame -> {:noreply, deliver(frame, state)}
    end
  end

  def handle_info({port, {:data, {:eol, "action " <> action}}}, %{port: port} = state) do
    Padwave.Bus.broadcast({:action, parse_action(action)})
    {:noreply, state}
  end

  def handle_info({port, {:data, {:eol, "down" <> _}}}, %{port: port} = state) do
    Logger.warning("[controller] input decoder reported a disconnect")
    {:noreply, state}
  end

  def handle_info({port, {:data, {:eol, "event " <> event}}}, %{port: port} = state) do
    case String.split(event, " ", trim: true) do
      ["activity"] -> Padwave.Bus.broadcast({:input, :activity})
      ["shake"] -> Padwave.Bus.broadcast({:input, :shake})
      ["trigger", side, edge] ->
        Padwave.Bus.broadcast(
          {:input, {:trigger, if(side == "l", do: :left, else: :right), String.to_existing_atom(edge)}}
        )
      _ -> :ok
    end

    {:noreply, state}
  end

  def handle_info({port, {:data, {:eol, "mic " <> hex}}}, %{port: port} = state) do
    case Base.decode16(hex, case: :mixed) do
      {:ok, payload} -> Padwave.Mic.ingest(payload)
      _ -> :ok
    end

    {:noreply, state}
  end

  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    Logger.warning("[controller] input decoder exited (#{status})")
    {:noreply, shutdown(state, :input_exited)}
  end

  def handle_info(_other, state), do: {:noreply, state}

  # -- internals -------------------------------------------------------------

  defp start_reader(state, info) do
    case find_binary() do
      nil ->
        Logger.warning("[controller] padwave binary not found; cannot read input")
        state

      path ->
        port =
          Port.open({:spawn_executable, path}, [
            :binary,
            :exit_status,
            {:line, 4096},
            {:args, ["input", info.node]}
          ])

        %{state | port: port, pending: info, info: nil}
    end
  end

  defp parse_action("seek " <> value), do: {:seek, String.to_integer(value)}
  defp parse_action("volume " <> value), do: {:volume, String.to_integer(value)}
  defp parse_action(name), do: String.to_atom(name)

  defp find_binary do
    System.find_executable("padwave") ||
      case Path.expand("~/.local/bin/padwave") do
        path -> if File.exists?(path), do: path, else: nil
      end
  end

  defp shutdown(%{info: nil, port: nil} = state, _reason), do: state

  defp shutdown(state, reason) do
    if state.port do
      try do
        Port.close(state.port)
      catch
        _, _ -> :ok
      end
    end

    if state.info do
      Logger.info("[controller] gone (#{reason})")
      Bus.broadcast({:device, :down, Map.put(state.info, :reason, reason)})
    end

    %{state | info: nil, pending: nil, port: nil}
  end

  defp deliver(frame, state) do
    case Process.whereis(Padwave.Input.Mapper) do
      nil -> :ok
      pid -> send(pid, {:frame, frame})
    end

    # Output modules (the charging sweep) watch frames without needing the same
    # direct hand-off the mapper gets.
    Bus.broadcast({:frame, frame}, :sensors)
    count(state)
  end

  # Reports per second, averaged over roughly a second. Useful for watching
  # power-save experiments: turning off sensors should drop this.
  defp count(state) do
    now = now()
    frames = state.frames + 1

    if now - state.since >= 1000 do
      rate = round(frames * 1000 / max(now - state.since, 1))
      %{state | frames: 0, since: now, rate: rate}
    else
      %{state | frames: frames}
    end
  end

  defp now, do: System.monotonic_time(:millisecond)

  @doc false
  def discover do
    "/sys/class/hidraw/hidraw*"
    |> Path.wildcard()
    |> Enum.sort()
    |> Enum.find_value(&probe/1)
  end

  defp probe(sysfs) do
    with {:ok, uevent} <- File.read(Path.join(sysfs, "device/uevent")),
         {:ok, bus, vendor, product} <- parse_id(uevent),
         true <- vendor == @sony and product in @supported do
      %{
        node: "/dev/" <> Path.basename(sysfs),
        sysfs: sysfs,
        name: capture(uevent, "HID_NAME") || "DualSense",
        mac: capture(uevent, "HID_UNIQ"),
        id: {vendor, product},
        model: if(product == @dualsense_edge, do: :edge, else: :dualsense),
        bus: bus,
        spec: if(bus == :usb, do: :input_usb, else: :input)
      }
    else
      _ -> nil
    end
  end

  defp parse_id(uevent) do
    case capture(uevent, "HID_ID") do
      nil ->
        nil

      value ->
        case String.split(value, ":") do
          [bus, vendor, product] ->
            with {b, ""} <- Integer.parse(bus, 16),
                 {v, ""} <- Integer.parse(vendor, 16),
                 {p, ""} <- Integer.parse(product, 16) do
              {:ok, if(b == 0x03, do: :usb, else: :bluetooth), v, p}
            else
              _ -> nil
            end

          _ ->
            nil
        end
    end
  end

  defp capture(uevent, key) do
    case Regex.run(~r/^#{key}=(.*)$/m, uevent) do
      [_, value] -> String.trim(value)
      _ -> nil
    end
  end
end
