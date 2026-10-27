defmodule Padwave.Output.Hid do
  @moduledoc """
  The 0x31 writer.

  The packet itself is built by `padwave output` (`scripts/padwave_output.cc`):
  this process only decides *what* changes and sends a line of named fields.
  The C++ side owns the report id, the sequence tag, the CRC and the field
  offsets, all from `padwave_schemas.hh`, so the byte layout has one definition
  and no Elixir code needs to know where a byte lives.

  Fields are the `OutputBody` members; a `valid_flag*` value is a comma list of
  flag names (`rumble`, `lightbar`, `player_leds`, …), exactly the names in the
  schema's enums. Binary fields (`*_trigger_params`) travel as hex.

  Everything goes through this one process, which serialises the command stream.
  Reports are only sent on change, a few per second at most.
  """

  use GenServer
  require Logger

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

  @doc "Sets the lightbar from a single RGB tuple."
  def lightbar({red, green, blue}), do: lightbar(red, green, blue)

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

    {:ok, %{writes: 0, errors: 0, recent: [], power_save: nil, player_leds: nil}}
  end

  @impl true
  def handle_cast({:lightbar, {red, green, blue}, opts}, state) do
    if Config.get(:lightbar_enabled, true) do
      emit(state, Report.lightbar_fields(red, green, blue, opts))
    else
      {:noreply, state}
    end
  end

  def handle_cast({:power_save, value}, state), do: emit(%{state | power_save: value}, %{})
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

  @doc "How many reports have been sent, for the debug endpoint."
  def stats, do: GenServer.call(__MODULE__, :stats, 2_000)

  @impl true
  def handle_call(:stats, _from, state),
    do: {:reply, Map.take(state, [:writes, :errors, :recent, :power_save, :player_leds]), state}

  @impl true
  def handle_info(_other, state), do: {:noreply, state}

  # Every report carries the persistent claims (currently power_save) on top of
  # whatever the caller asked for, so the controller state does not depend on
  # which feature wrote last.
  defp emit(state, fields) do
    send_line(state, "fields " <> serialize(merge_persistent(fields, state)))
  end

  defp merge_persistent(fields, %{power_save: nil}), do: fields

  defp merge_persistent(fields, %{power_save: value}) do
    fields
    |> Map.update(:valid_flag1, [:power_save], &claim(&1, :power_save))
    |> Map.put(:power_save_control, value)
  end

  defp claim(flags, name) when is_list(flags), do: [name | flags]
  defp claim(flags, _name) when is_integer(flags), do: flags

  defp send_line(state, line) do
    Padwave.Player.Native.command(line)
    recent = [%{seq: state.writes, hex: line} | state.recent] |> Enum.take(4)
    {:noreply, %{state | writes: state.writes + 1, recent: recent}}
  end

  defp serialize(fields) do
    Enum.map_join(fields, ";", fn {key, value} -> "#{key}=#{encode(value)}" end)
  end

  defp encode(value) when is_integer(value), do: Integer.to_string(value)
  defp encode(value) when is_atom(value), do: Atom.to_string(value)
  defp encode(value) when is_binary(value), do: Base.encode16(value, case: :lower)

  defp encode(value) when is_list(value) do
    if Enum.all?(value, &is_atom/1) do
      Enum.map_join(value, ",", &Atom.to_string/1)
    else
      Enum.map_join(value, ",", &to_string/1)
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

      if lit?, do: Bitwise.bor(mask, Bitwise.bsl(1, index - 1)), else: mask
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
end
