defmodule Padwave.Output.Hid do
  @moduledoc """
  Writes DualSense output reports straight to the controller's hidraw node.

  The kernel's `playstation` driver owns rumble and exposes the lightbar through
  sysfs, but it does not expose the adaptive triggers or their actuators at all.
  Those live in the same report `0x31` we already know how to frame and sign, so
  padwave writes that report itself — for the lightbar (instead of sysfs) and
  for trigger effects.

  Everything goes through this one process, which serialises writes and owns
  the Bluetooth report counter (the high nibble of the sequence tag, 0..15).
  Writes are only issued on change (a few per second at most), so this never
  competes with the audio stream for meaningful bandwidth.
  """

  use GenServer
  require Logger

  alias Padwave.{Bus, Config}
  alias Padwave.Output.Report

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc "Sets the lightbar colour."
  def lightbar(red, green, blue),
    do: GenServer.cast(__MODULE__, {:lightbar, {red, green, blue}})

  @doc "Sets one trigger's effect (`0x00` off, `0x06` vibration, …)."
  def trigger(side, mode, params \\ []),
    do: GenServer.cast(__MODULE__, {:trigger, side, mode, params})

  @impl true
  def init(_opts) do
    Bus.subscribe()
    {:ok, %{fd: nil, seq: 0, writes: 0, errors: 0}}
  end

  @impl true
  def handle_cast({:lightbar, {red, green, blue}}, state) do
    if Config.get(:lightbar_enabled, true) do
      write(state, Report.lightbar(red, green, blue, seq: state.seq))
    else
      {:noreply, state}
    end
  end

  def handle_cast({:trigger, side, mode, params}, state) do
    write(state, Report.trigger(side, mode, params, seq: state.seq))
  end

  @doc "How many reports have been written, for the debug endpoint."
  def stats, do: GenServer.call(__MODULE__, :stats, 2_000)

  @impl true
  def handle_call(:stats, _from, state), do: {:reply, Map.take(state, [:writes, :errors, :seq]), state}

  @impl true
  def handle_info({:padwave, {:device, :down, _info}}, state) do
    if state.fd, do: :file.close(state.fd)
    {:noreply, %{state | fd: nil}}
  end

  def handle_info(_other, state), do: {:noreply, state}

  defp write(state, report) do
    case open(state) do
      {:ok, state} ->
        case :file.write(state.fd, report) do
          :ok ->
            {:noreply, %{state | seq: rem(state.seq + 1, 16), writes: state.writes + 1}}

          {:error, reason} ->
            Logger.warning("[hid] output report failed: #{inspect(reason)}")
            :file.close(state.fd)
            {:noreply, %{state | fd: nil, errors: state.errors + 1}}
        end

      {:error, state} ->
        {:noreply, state}
    end
  end

  defp open(%{fd: nil} = state) do
    case Padwave.Input.Device.node() do
      nil ->
        {:error, state}

      node ->
        case :file.open(node, [:write, :raw, :binary]) do
          {:ok, fd} ->
            {:ok, %{state | fd: fd}}

          {:error, reason} ->
            Logger.warning("[hid] cannot open #{node} for writing: #{inspect(reason)}")
            {:error, state}
        end
    end
  end

  defp open(state), do: {:ok, state}
end
