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

  @doc "A lightbar update. `seq` is the 4-bit report counter (0..15)."
  def lightbar(red, green, blue, seq \\ 0, extra \\ %{}) do
    build(
      Map.merge(
        %{
          seq_tag: band(seq, 0x0F) <<< 4,
          tag: @tag,
          valid_flag1: @valid_flag1_lightbar,
          lightbar_red: red,
          lightbar_green: green,
          lightbar_blue: blue
        },
        extra
      )
    )
  end

  @doc "A player-LED update. `mask` uses bits 0..4 for the five LEDs."
  def player_leds(mask, seq \\ 0) do
    build(%{
      seq_tag: band(seq, 0x0F) <<< 4,
      tag: @tag,
      valid_flag1: @valid_flag1_player_leds,
      player_leds: mask
    })
  end

  @doc """
  Builds a report from named fields (see `Padwave.Schema.DualSenseOutput`).
  Unset fields are zero and the CRC is computed and appended.
  """
  def build(fields) when is_map(fields) do
    spec = DualSenseOutput.spec(:output)
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

    data = binary_part(body, 0, spec.size - 4)
    data <> <<crc(data)::little-32>>
  end

  @doc "CRC of an output report body (everything before the four CRC bytes)."
  def crc(data) when is_binary(data) do
    :erlang.crc32(<<@seed>> <> data)
  end

  defp put_byte(binary, index, value) when value in 0..255 do
    <<head::binary-size(index), _old, tail::binary>> = binary
    <<head::binary, value, tail::binary>>
  end
end
