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

  import Bitwise, only: [band: 2, bor: 2, bsl: 2]

  alias Padwave.{Bus, Config}
  alias Padwave.Output.Report

  def start_link(_opts), do: GenServer.start_link(__MODULE__, nil, name: __MODULE__)

  @doc """
  Sets the lightbar colour.

  Options: `:brightness` also claims `valid_flag2` bit 0 and sends
  `led_brightness`, which a colour-only report does not.
  """
  def lightbar(red, green, blue, opts \\ []),
    do: GenServer.cast(__MODULE__, {:lightbar, {red, green, blue}, opts})

  @doc "Sends a report built from a map of output-report field names."
  def send_fields(fields), do: GenServer.cast(__MODULE__, {:fields, fields})

  @doc "Sets the five white player LEDs from a 5-bit mask."
  def player_leds(mask), do: GenServer.cast(__MODULE__, {:player_leds, mask})

  @doc "Puts the player LEDs back to whatever the kernel has set."
  def reset_player_leds, do: GenServer.cast(__MODULE__, :reset_player_leds)

  @doc """
  Makes `power_save_control` a persistent claim: every report this writer sends
  from now on also sets `valid_flag1` bit 1 and the byte. `nil` stops claiming.
  """
  def set_power_save(nil), do: GenServer.cast(__MODULE__, :clear_power_save)
  def set_power_save(value), do: GenServer.cast(__MODULE__, {:power_save, value})

  @doc "Sets one trigger's effect (`0x00` off, `0x06` vibration, …)."
  def trigger(side, mode, params \\ []),
    do: GenServer.cast(__MODULE__, {:trigger, side, mode, params})

  @impl true
  def init(_opts) do
    Bus.subscribe()
    {:ok, %{fd: nil, seq: 0, writes: 0, errors: 0, recent: [], power_save: nil, player_leds: nil}}
  end

  @impl true
  def handle_cast({:lightbar, {red, green, blue}, opts}, state) do
    if Config.get(:lightbar_enabled, true) do
      emit(state, Report.lightbar_fields(red, green, blue, opts))
    else
      {:noreply, state}
    end
  end

  def handle_cast({:power_save, value}, state) do
    emit(%{state | power_save: value}, %{})
  end

  def handle_cast(:clear_power_save, state), do: {:noreply, %{state | power_save: nil}}

  def handle_cast({:player_leds, mask}, state),
    do: emit(%{state | player_leds: mask}, Report.player_leds_fields(mask))

  def handle_cast(:reset_player_leds, state) do
    mask = kernel_player_led_mask()
    emit(%{state | player_leds: mask}, Report.player_leds_fields(mask))
  end

  def handle_cast({:trigger, side, mode, params}, state),
    do: emit(state, Report.trigger_fields(side, mode, params))

  def handle_cast({:fields, fields}, state) do
    emit(state, atomize(fields))
  rescue
    error ->
      Logger.warning("[hid] cannot build that report: #{Exception.message(error)}")
      {:noreply, state}
  end

  @doc "How many reports have been written, for the debug endpoint."
  def stats, do: GenServer.call(__MODULE__, :stats, 2_000)

  @impl true
  def handle_call(:stats, _from, state),
    do: {:reply, Map.take(state, [:writes, :errors, :seq, :recent, :power_save, :player_leds]), state}

  @impl true
  def handle_info({:padwave, {:device, :down, _info}}, state) do
    if state.fd, do: :file.close(state.fd)
    {:noreply, %{state | fd: nil}}
  end

  def handle_info(_other, state), do: {:noreply, state}

  # Every report carries the persistent claims (currently power_save) on top of
  # whatever the caller asked for, so the controller state does not depend on
  # which feature wrote last.
  defp emit(state, fields) do
    write(state, Report.build(merge_persistent(fields, state), :bluetooth, seq: state.seq))
  end

  defp merge_persistent(fields, %{power_save: nil}), do: fields

  defp merge_persistent(fields, %{power_save: value}) do
    fields
    |> Map.update(:valid_flag1, [:power_save], &claim(&1, :power_save))
    |> Map.put(:power_save_control, value)
  end

  defp claim(flags, name) when is_list(flags), do: [name | flags]
  defp claim(flags, _name) when is_integer(flags), do: flags

  defp write(state, report) do
    case open(state) do
      {:ok, state} ->
        case :file.write(state.fd, report) do
          :ok ->
            entry = %{seq: state.seq, hex: Base.encode16(report, case: :lower)}
            recent = [entry | state.recent] |> Enum.take(4)

            {:noreply, %{state | seq: rem(state.seq + 1, 16), writes: state.writes + 1, recent: recent}}

          {:error, reason} ->
            Logger.warning("[hid] output report failed: #{inspect(reason)}")
            :file.close(state.fd)
            {:noreply, %{state | fd: nil, errors: state.errors + 1}}
        end

      {:error, state} ->
        {:noreply, state}
    end
  end

  # Reads the kernel's current player-LED state so "reset" can restore it:
  # each LED's brightness is 0 or 1 and maps to one bit of the output mask.
  defp kernel_player_led_mask do
    "/sys/class/leds/*:white:player-*"
    |> Path.wildcard()
    |> Enum.filter(fn dir ->
      case File.read_link(dir) do
        {:ok, target} -> String.contains?(target, "054C:0CE6")
        _ -> false
      end
    end)
    |> Enum.reduce(0, fn dir, mask ->
      index = dir |> Path.basename() |> String.split("player-") |> List.last() |> String.to_integer()

      lit? =
        case File.read(Path.join(dir, "brightness")) do
          {:ok, value} -> String.trim(value) not in ["", "0"]
          _ -> false
        end

      if lit?, do: bor(mask, bsl(1, index - 1)), else: mask
    end)
  end

  defp atomize(fields) do
    Map.new(fields, fn
      {key, value} when is_binary(key) -> {String.to_existing_atom(key), normalize(value)}
      {key, value} -> {key, normalize(value)}
    end)
  end

  # JSON gives us strings: a list of them is a set of flag-bit names, a list of
  # integers is raw bytes for a binary field.
  defp normalize(value) when is_list(value) do
    if Enum.all?(value, &is_binary/1) do
      Enum.map(value, fn name -> String.to_existing_atom(name) end)
    else
      :erlang.list_to_binary(value)
    end
  end

  defp normalize(value), do: value

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
