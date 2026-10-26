defmodule Padwave.Input.Device do
  @moduledoc """
  Finds the DualSense's `hidraw` node, reads input reports from it, and keeps
  re-scanning so that a controller that is switched off and back on is picked
  up again without the service restarting.

  The node is opened read-only, by the reader process itself (Erlang's raw file
  handles belong to the process that opened them). The reader is killed when
  the controller goes away, which closes the descriptor.

  LinuxAudio4Dualsense5 owns the write side (speaker audio) and the kernel's
  `playstation` driver owns rumble/lightbar, so a read-only reader is the
  friendly neighbour here.
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
       reader: nil,
       ref: nil,
       spec: :input,
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
        if state.info && state.info.node == node do
          {:noreply, %{state | info: info}}
        else
          {:noreply, state |> shutdown(:switched) |> start_reader(info)}
        end
    end
  end

  def handle_info({:hid_opened, pid}, %{reader: pid, pending: info} = state) when info != nil do
    Logger.info("[controller] #{info.node} is a #{info.name} (#{info.mac}), reading")
    Bus.broadcast({:device, :up, info})
    {:noreply, %{state | info: info, pending: nil}}
  end

  def handle_info({:hid_opened, _pid}, state), do: {:noreply, state}

  def handle_info({:hid_open_failed, reason}, state) do
    Logger.warning("[controller] cannot open hidraw: #{inspect(reason)}")
    {:noreply, %{state | reader: nil, ref: nil, pending: nil, info: nil}}
  end

  def handle_info({:hid, bin}, state) do
    case Frame.from_report(bin, state.spec || :input) do
      {:ok, frame} ->
        {:noreply, deliver(frame, state)}

      {:error, :bad_crc} ->
        {:noreply, %{state | dropped: state.dropped + 1}}

      {:error, _reason} ->
        {:noreply, state}
    end
  end

  def handle_info({:hid_odd, size}, state) do
    Logger.warning("[controller] unexpected report size #{size}; expected one whole 0x31 report")
    {:noreply, state}
  end

  def handle_info({:hid_closed, reason}, state) do
    Logger.warning("[controller] hidraw read failed: #{inspect(reason)}")
    {:noreply, shutdown(state, :read_failed)}
  end

  def handle_info({:DOWN, ref, :process, _pid, reason}, %{ref: ref} = state) do
    if state.info, do: Logger.warning("[controller] reader exited: #{inspect(reason)}")
    {:noreply, shutdown(state, :reader_exit)}
  end

  def handle_info(_other, state), do: {:noreply, state}

  # -- internals -------------------------------------------------------------

  defp start_reader(state, info) do
    parent = self()
    {reader, ref} = spawn_monitor(fn -> reader_main(info.node, info.spec, parent) end)
    # Mic frames are shorter than the state report (and would be truncated by
    # its fixed-size read), so they get their own fd and their own buffer.
    spawn_link(fn -> mic_reader_main(info.node) end)
    %{state | reader: reader, ref: ref, pending: info, info: nil, spec: info.spec}
  end

  defp shutdown(%{info: nil, reader: nil} = state, _reason), do: state

  defp shutdown(state, reason) do
    if state.reader, do: Process.exit(state.reader, :kill)

    if state.info do
      Logger.info("[controller] gone (#{reason})")
      Bus.broadcast({:device, :down, Map.put(state.info, :reason, reason)})
    end

    %{state | info: nil, pending: nil, reader: nil, ref: nil}
  end

  defp reader_main(node, spec, parent) do
    # hidraw delivers exactly one report per read(); asking for one report's
    # worth of bytes keeps Erlang from concatenating several reports into one
    # buffer (which is what happens if you ask for 512).
    size = Padwave.Schema.DualSenseInput.spec(spec).size

    case :file.open(node, [:read, :raw, :binary]) do
      {:ok, fd} ->
        send(parent, {:hid_opened, self()})
        read_loop(fd, parent, size)

      {:error, reason} ->
        send(parent, {:hid_open_failed, reason})
    end
  end

  defp read_loop(fd, parent, size) do
    case :file.read(fd, size) do
      {:ok, bin} when is_binary(bin) and byte_size(bin) == size ->
        send(parent, {:hid, bin})
        read_loop(fd, parent, size)

      {:ok, bin} when is_binary(bin) and byte_size(bin) > 0 ->
        send(parent, {:hid_odd, byte_size(bin)})
        read_loop(fd, parent, size)

      {:ok, _empty} ->
        read_loop(fd, parent, size)

      {:error, reason} ->
        send(parent, {:hid_closed, reason})
    end
  rescue
    error ->
      send(parent, {:hid_closed, error})
  end

  defp mic_reader_main(node) do
    case :file.open(node, [:read, :raw, :binary]) do
      {:ok, fd} -> mic_loop(fd)
      {:error, _reason} -> :ok
    end
  end

  defp mic_loop(fd) do
    case :file.read(fd, 256) do
      {:ok, bin} when is_binary(bin) and byte_size(bin) > 0 ->
        if Padwave.Schema.DualSenseMic.frame?(bin) do
          Padwave.Mic.ingest(Padwave.Schema.DualSenseMic.payload(bin))
        end

        mic_loop(fd)

      {:ok, _empty} ->
        mic_loop(fd)

      {:error, _reason} ->
        :ok
    end
  rescue
    _error -> :ok
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
