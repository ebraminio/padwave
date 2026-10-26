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
  @doc """
  A lightbar update.

  Options: `:seq` (Bluetooth report counter 0..15), `:bus` (`:bluetooth` or
  `:usb`), `:extra` (further fields, e.g. player LEDs).
  """
  @doc false
  def lightbar_fields(red, green, blue, opts \\ []) do
    fields = %{
      valid_flag1: [:lightbar],
      lightbar_red: red,
      lightbar_green: green,
      lightbar_blue: blue
    }

    case Keyword.get(opts, :brightness) do
      nil -> fields
      value -> Map.merge(fields, %{valid_flag2: [:brightness], led_brightness: value})
    end
  end

  def lightbar(red, green, blue, opts \\ []) when is_list(opts) do
    bus = Keyword.get(opts, :bus, :bluetooth)

    fields =
      lightbar_fields(red, green, blue, opts)
      |> Map.merge(Keyword.get(opts, :extra, %{}))

    build(fields, bus, seq: Keyword.get(opts, :seq, 0))
  end

  @doc "A player-LED update. `mask` uses bits 0..4 for the five LEDs."
  @doc false
  def player_leds_fields(mask), do: %{valid_flag1: [:player_leds], player_leds: mask}

  def player_leds(mask, opts \\ []) when is_list(opts) do
    build(
      player_leds_fields(mask),
      Keyword.get(opts, :bus, :bluetooth),
      seq: Keyword.get(opts, :seq, 0)
    )
  end

  @doc """
  A trigger effect: `mode` plus up to ten parameters.

  The official mode bytes are `0x05` off, `0x21` feedback, `0x25` weapon and
  `0x26` vibration; the parameters are zone masks and 3-bit strengths (see
  `Padwave.Output.TriggerEffects`, which builds them). `0x00` and `0x06` exist
  in old probing code but are not what current firmware expects.

  Enabled by `valid_flag0` bits `:right_trigger` and `:left_trigger`.
  """
  @doc false
  def trigger_fields(side, mode, params \\ []) when side in [:left, :right] do
    case side do
      :right ->
        %{
          valid_flag0: [:right_trigger],
          right_trigger_mode: mode,
          right_trigger_params: trigger_params(params)
        }

      :left ->
        %{
          valid_flag0: [:left_trigger],
          left_trigger_mode: mode,
          left_trigger_params: trigger_params(params)
        }
    end
  end

  def trigger(side, mode, params \\ [], opts \\ []) when side in [:left, :right] do
    build(
      trigger_fields(side, mode, params),
      Keyword.get(opts, :bus, :bluetooth),
      seq: Keyword.get(opts, :seq, 0)
    )
  end

  @doc """
  Builds a report from named fields (see `Padwave.Schema.DualSenseOutput`).

  Unset fields are zero. Bluetooth reports get the sequence tag and the CRC;
  USB reports are just the id plus the shared payload (no CRC — the kernel only
  signs the Bluetooth transport).
  """
  def build(fields, bus \\ :bluetooth, opts \\ []) when is_map(fields) do
    spec = DualSenseOutput.spec(if bus == :usb, do: :output_usb, else: :output)

    fields =
      if bus == :usb do
        fields
      else
        Map.merge(header_defaults(Keyword.get(opts, :seq, 0)), fields)
      end

    offsets = Map.new(spec.fields, fn field -> {field.name, field.offset} end)
    sizes = Map.new(spec.fields, fn field -> {field.name, field.size} end)
    bit_layouts = Map.new(spec.fields, fn field -> {field.name, Map.get(field, :bits, [])} end)

    zeroed =
      :binary.copy(<<0>>, spec.size)
      |> put_byte(0, spec.id)

    body =
      Enum.reduce(fields, zeroed, fn {name, value}, acc ->
        offset =
          Map.get(offsets, name) ||
            raise ArgumentError, "unknown output report field #{inspect(name)}"

        resolved = resolve_bits(value, Map.get(bit_layouts, name, []))
        put_value(acc, offset, resolved, Map.get(sizes, name, 1))
      end)

    if bus == :usb do
      body
    else
      data = binary_part(body, 0, spec.size - 4)
      data <> <<crc(data)::little-32>>
    end
  end

  defp header_defaults(seq), do: %{seq_tag: band(seq, 0x0F) <<< 4, tag: @tag}

  @doc "CRC of an output report body (everything before the four CRC bytes)."
  def crc(data) when is_binary(data) do
    :erlang.crc32(<<@seed>> <> data)
  end

  # A bit-group field takes the raw integer, a list of one-bit names to set, or
  # a map of name => value for groups wider than one bit.
  defp resolve_bits(value, []), do: value
  defp resolve_bits(value, _bits) when is_integer(value), do: value
  defp resolve_bits(names, bits) when is_list(names), do: encode_bits(bits, names)
  defp resolve_bits(values, bits) when is_map(values), do: encode_bits(bits, values)

  defp encode_bits(bits, names) when is_list(names) do
    Enum.reduce(names, 0, fn name, acc ->
      case Enum.find(bits, &(&1.name == name)) do
        %{offset: offset, width: 1} ->
          Bitwise.bor(acc, Bitwise.bsl(1, offset))

        %{width: width} ->
          raise ArgumentError,
                "bit #{inspect(name)} is #{width} bits wide; pass a map instead"

        nil ->
          raise ArgumentError,
                "unknown bit #{inspect(name)}; known: #{inspect(Enum.map(bits, & &1.name))}"
      end
    end)
  end

  defp encode_bits(bits, values) when is_map(values) do
    Enum.reduce(values, 0, fn {name, value}, acc ->
      case Enum.find(bits, &(&1.name == name)) do
        %{offset: offset, width: 1} ->
          if value in [true, 1], do: Bitwise.bor(acc, Bitwise.bsl(1, offset)), else: acc

        %{offset: offset, width: width} ->
          mask = Bitwise.bsl(1, width) - 1
          Bitwise.bor(acc, Bitwise.bsl(Bitwise.band(value, mask), offset))

        nil ->
          raise ArgumentError,
                "unknown bit #{inspect(name)}; known: #{inspect(Enum.map(bits, & &1.name))}"
      end
    end)
  end

  defp put_value(binary, index, value, 1) when is_integer(value) and value in 0..0xFF,
    do: put_byte(binary, index, value)

  defp put_value(binary, index, value, 2) when is_integer(value) and value in 0..0xFFFF,
    do: put_bytes(binary, index, <<value::little-16>>)

  defp put_value(binary, index, value, 4) when is_integer(value) and value in 0..0xFFFFFFFF,
    do: put_bytes(binary, index, <<value::little-32>>)

  defp put_value(binary, index, value, size) when is_binary(value) and byte_size(value) == size,
    do: put_bytes(binary, index, value)

  defp put_bytes(binary, index, bytes) do
    <<head::binary-size(index), _::binary-size(byte_size(bytes)), tail::binary>> = binary
    <<head::binary, bytes::binary, tail::binary>>
  end

  defp trigger_params(params) when is_list(params) do
    params |> Enum.map(&<<&1::8>>) |> IO.iodata_to_binary() |> trigger_params()
  end

  defp trigger_params(params) when is_binary(params) do
    take = min(byte_size(params), 10)
    binary_part(params, 0, take) <> :binary.copy(<<0>>, 10 - take)
  end

  defp put_byte(binary, index, value) when value in 0..255 do
    <<head::binary-size(index), _old, tail::binary>> = binary
    <<head::binary, value, tail::binary>>
  end
end
