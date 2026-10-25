defmodule Padwave.Output.Report do
  @moduledoc """
  Builds DualSense output reports.

  Bluetooth output reports are signed: the last four bytes are a CRC-32 over
  the report with `0xA2` folded in as a seed. The kernel does

      crc = crc32_le(0xFFFFFFFF, &seed, 1);          // seed = 0xA2
      crc = ~crc32_le(crc, report->data, report->len - 4);

  which is the same as a standard reflected CRC-32 (init/final `0xFFFFFFFF`)
  over `<<0xA2>> <> report[0..len-5]`, stored little-endian.

  Field offsets come from `Padwave.Schema.DualSenseOutput`, so the packet
  description and the encoder cannot drift apart.
  """

  import Bitwise

  alias Padwave.Schema.DualSenseOutput

  @seed 0xA2
  @tag 0x10
  @valid_flag1_lightbar 0x04
  @valid_flag1_player_leds 0x10

  @doc """
  A lightbar update.

  Options: `:seq` (Bluetooth report counter 0..15), `:bus` (`:bluetooth` or
  `:usb`), `:extra` (further fields, e.g. player LEDs).
  """
  def lightbar(red, green, blue, opts \\ []) when is_list(opts) do
    fields = %{
      valid_flag1: @valid_flag1_lightbar,
      lightbar_red: red,
      lightbar_green: green,
      lightbar_blue: blue
    }

    fields = Map.merge(fields, Keyword.get(opts, :extra, %{}))
    bus = Keyword.get(opts, :bus, :bluetooth)
    build(header(fields, Keyword.get(opts, :seq, 0), bus), bus)
  end

  @doc "A player-LED update. `mask` uses bits 0..4 for the five LEDs."
  def player_leds(mask, opts \\ []) when is_list(opts) do
    bus = Keyword.get(opts, :bus, :bluetooth)

    build(
      header(
        %{valid_flag1: @valid_flag1_player_leds, player_leds: mask},
        Keyword.get(opts, :seq, 0),
        bus
      ),
      bus
    )
  end

  @doc """
  Builds a report from named fields (see `Padwave.Schema.DualSenseOutput`).

  Unset fields are zero. Bluetooth reports get the sequence tag and the CRC;
  USB reports are just the id plus the shared payload (no CRC — the kernel only
  signs the Bluetooth transport).
  """
  def build(fields, bus \\ :bluetooth) when is_map(fields) do
    spec = DualSenseOutput.spec(if bus == :usb, do: :output_usb, else: :output)
    offsets = Map.new(spec.fields, fn field -> {field.name, field.offset} end)

    zeroed =
      :binary.copy(<<0>>, spec.size)
      |> put_byte(0, spec.id)

    body =
      Enum.reduce(fields, zeroed, fn {name, value}, acc ->
        offset =
          Map.get(offsets, name) ||
            raise ArgumentError, "unknown output report field #{inspect(name)}"

        put_byte(acc, offset, value)
      end)

    if bus == :usb do
      body
    else
      data = binary_part(body, 0, spec.size - 4)
      data <> <<crc(data)::little-32>>
    end
  end

  defp header(fields, seq, :bluetooth) do
    Map.merge(%{seq_tag: band(seq, 0x0F) <<< 4, tag: @tag}, fields)
  end

  defp header(fields, _seq, :usb), do: fields

  @doc "CRC of an output report body (everything before the four CRC bytes)."
  def crc(data) when is_binary(data) do
    :erlang.crc32(<<@seed>> <> data)
  end

  defp put_byte(binary, index, value) when value in 0..255 do
    <<head::binary-size(index), _old, tail::binary>> = binary
    <<head::binary, value, tail::binary>>
  end
end
