defmodule Padwave.Wire do
  @moduledoc """
  Declarative descriptions of binary packets.

  The point of this module is that a controller report should be *described*
  rather than hand-decoded with a pile of `:binary.at/2` calls. A description
  compiles into

    * `decode_<name>/1` — one binary match, no copying, compile-time checked
      against the declared packet size;
    * `spec(:<name>)` — the same layout as plain data, which the web UI renders
      as a live packet map.

  ## Example

      defmodule MySchema do
        use Padwave.Wire

        packet :input, 0x31, size: 6 do
          u8    :seq
          u8    :left_x
          bits  :buttons, dpad: 4, square: 1, cross: 1, circle: 1, triangle: 1
          i16le :temperature
        end
      end

      MySchema.decode_input(<<0x31, 1, 127, 0x48, 0x01, 0x00>>)
      #=> %{seq: 1, left_x: 127, buttons: %{dpad: 8, square: false, ...}, temperature: 1}

  Multi-byte integers name their byte order (`i16le`, `u32le`) so the
  description says what it means; `u8` has none to state. Bit groups read the
  low bit first.

  Fields are laid out in declaration order starting at byte 0. Declare the
  packet id as the first pattern byte; the remaining fields must add up to
  `size - 1`. Use `skip n` for bytes that are not (yet) understood — that is
  how the map stays honest about what is known and what is not.

  Bit groups are little-endian by convention here (low bit first), which is
  what the DualSense uses: the d-pad lives in the low nibble and the face
  buttons in the high nibble.
  """

  import Bitwise

  @doc false
  defmacro __using__(_opts) do
    quote do
      import Padwave.Wire, only: [packet: 3, packet: 4, field_list: 2]
      import Bitwise
    end
  end

  @doc """
  Declares a named field list that packets can `splice` in.

  The list lives in its own small module so that it is compiled before the
  packets that reference it (Elixir applies `@attr` writes after macro
  expansion, so a same-module lookup would not see it).

  The DualSense's USB and Bluetooth reports are the same payload behind
  different framing, so the payload is written once, with a comment per field,
  and referenced by each packet:

      field_list :input_body do
        u8 :lx                 # +0x00 left stick X
        ...
      end

      packet :input, 0x31, size: 78 do
        u8 :seq_tag            # 0x01
        splice MySchema.Body, :input_body
        u32 :crc
      end

      packet :input_usb, 0x01, size: 64 do
        splice MySchema.Body, :input_body
      end

  The splice is resolved at compile time: the generated decoder is still one
  binary match over the whole report.
  """
  defmacro field_list(name, do: block) do
    data = fields(block, __CALLER__)

    quote do
      @doc false
      def __wire_fields__(unquote(name)), do: unquote(Macro.escape(data))
    end
  end

  @doc """
  Describes a packet.

      packet :input, 0x31, size: 78 do
        ...
      end
  """
  defmacro packet(name, id, opts, do: block) do
    build_packet(name, id, opts, block, __CALLER__)
  end

  defmacro packet(name, id, do: block) do
    build_packet(name, id, [], block, __CALLER__)
  end

  # -- compile time ----------------------------------------------------------

  defp build_packet(name, id, opts, block, env) do
    unless is_atom(name) do
      raise ArgumentError, "packet name must be an atom, got: #{inspect(name)}"
    end

    unless is_integer(id) do
      raise ArgumentError, "packet id must be an integer, got: #{inspect(id)}"
    end

    size = Keyword.get(opts, :size) || raise ArgumentError, "packet #{name} needs a :size"
    description = Keyword.get(opts, :doc)

    declared =
      block
      |> fields(env)
      |> layout()

    used = 1 + Enum.reduce(declared, 0, fn f, acc -> acc + f.size end)

    if used != size do
      raise ArgumentError,
            "packet #{inspect(name)} declares size #{size} but its fields describe #{used} bytes " <>
              "(the id byte counts as one)"
    end

    {pattern, prelude, pairs} = codegen(declared)
    decode = :"decode_#{name}"

    spec = %{
      name: name,
      id: id,
      size: size,
      doc: description,
      fields: declared
    }

    quote do
      @doc unquote(description || "Decoded `#{name}` packet.")
      def unquote(decode)(<<unquote(id), unquote_splicing(pattern)>> = __padwave_bin__)
          when byte_size(__padwave_bin__) == unquote(size) do
        unquote_splicing(prelude)
        %{unquote_splicing(pairs)}
      end

      def unquote(decode)(other) when is_binary(other) do
        {:error, {:unexpected_report, byte_size(other), <<binary_part(other, 0, 1)::binary>>}}
      end

      @doc false
      def spec(unquote(name)), do: unquote(Macro.escape(spec))
    end
  end

  # Turns the block into a flat list of field declarations.
  defp fields({:__block__, _, statements}, env) do
    enums =
      Enum.reduce(statements, %{}, fn
        {:enum, _, [name, pairs]}, acc when is_atom(name) and is_list(pairs) ->
          Map.put(acc, name, Map.new(pairs))

        _, acc ->
          acc
      end)

    statements
    |> Enum.reject(&match?({:enum, _, _}, &1))
    |> Enum.flat_map(&fields(&1, env))
    |> attach_enums(enums)
  end

  defp fields({:u8, _, [name]}, _env) when is_atom(name), do: [{:u8, name}]

  # Flag bytes declare their bits by position, like a C enum of 1<<n values:
  # `u8 :valid_flag0, flags: [rumble: 0, haptics: 1, ...]`. Decoding still
  # yields the byte, but the spec carries the names and the encoder accepts
  # them (`%{valid_flag0: [:rumble, :right_trigger]}`).
  defp fields({:u8, _, [name, opts]}, _env) when is_atom(name) and is_list(opts) do
    [{:u8, name, normalize_flags(Keyword.get(opts, :flags, []), 7)}]
  end

  defp fields({:u16le, _, [name]}, _env) when is_atom(name), do: [{:u16le, name}]

  defp fields({:u16le, _, [name, opts]}, _env) when is_atom(name) and is_list(opts) do
    [{:u16le, name, normalize_flags(Keyword.get(opts, :flags, []), 15)}]
  end

  defp fields({:i16le, _, [name]}, _env) when is_atom(name), do: [{:i16le, name}]
  defp fields({:u32le, _, [name]}, _env) when is_atom(name), do: [{:u32le, name}]

  defp fields({:bytes, _, [name, count]}, _env) when is_atom(name) and is_integer(count) do
    [{:bytes, name, count}]
  end

  defp fields({:skip, _, [count]}, _env) when is_integer(count), do: [{:skip, count}]

  defp fields({:bits, _, [name, bits]}, _env) when is_atom(name) and is_list(bits) do
    [{:bits, name, bits}]
  end

  defp fields({:splice, _, [module_ast, name]}, env) when is_atom(name) do
    module = Macro.expand(module_ast, env)
    Code.ensure_compiled!(module)

    if function_exported?(module, :__wire_fields__, 1) do
      module.__wire_fields__(name)
    else
      raise ArgumentError,
            "#{inspect(module)} has no field list #{inspect(name)}; declare one with " <>
              "`field_list #{name} do ... end`"
    end
  end

  defp attach_enums(parsed, enums) do
    Enum.map(parsed, fn
      {:bits, name, bits} ->
        bits =
          Enum.map(bits, fn
            {bit_name, width, enum} -> {bit_name, width, enum}
            {bit_name, width} -> {bit_name, width, Map.get(enums, bit_name)}
          end)

        {:bits, name, bits}

      other ->
        other
    end)
  end

  defp fields(other, _env) do
    raise ArgumentError, """
    cannot understand packet field: #{Macro.to_string(other)}

    Supported fields are:
      u8 :name
      u8 :name, flags: [bit_name: bit_position, ...]
      u16le :name   i16le :name   u32le :name
      u16le :name, flags: [bit_name: bit_position, ...]
      bytes :name, count
      skip count
      bits :name, low: 4, high: 1, ...
      enum :name, value_atom: 0, ...      (inside the same do-block, names a bits field)
    """
  end

  # Assigns byte offsets and validates bit widths. Offsets are absolute in the
  # report, so the first field starts at 1 (byte 0 is the report id).
  defp layout(fields) do
    {laid_out, _next} =
      Enum.map_reduce(fields, 1, fn field, offset ->
        field = lay_out_field(field, offset)
        {field, offset + field.size}
      end)

    laid_out
  end

  defp lay_out_field({:u8, name}, offset), do: %{kind: :u8, name: name, offset: offset, size: 1}

  defp lay_out_field({:u8, name, flags}, offset),
    do: %{kind: :u8, name: name, offset: offset, size: 1, flags: flags}

  # Stored as a map so `spec/1` is JSON-friendly; the declaration stays a
  # readable keyword list.
  defp normalize_flags(flags, max_bit) when is_list(flags) do
    Map.new(flags, fn
      {name, bit} when is_atom(name) and is_integer(bit) and bit in 0..max_bit ->
        {name, bit}

      other ->
        raise ArgumentError,
              "flag bits are `name: bit_position` pairs (0..#{max_bit}), got #{inspect(other)}"
    end)
  end

  defp lay_out_field({:u16le, name}, offset), do: %{kind: :u16le, name: name, offset: offset, size: 2}

  defp lay_out_field({:u16le, name, flags}, offset),
    do: %{kind: :u16le, name: name, offset: offset, size: 2, flags: flags}

  defp lay_out_field({:i16le, name}, offset), do: %{kind: :i16le, name: name, offset: offset, size: 2}
  defp lay_out_field({:u32le, name}, offset), do: %{kind: :u32le, name: name, offset: offset, size: 4}

  defp lay_out_field({:bytes, name, n}, offset),
    do: %{kind: :bytes, name: name, offset: offset, size: n}

  defp lay_out_field({:skip, n}, offset), do: %{kind: :skip, name: nil, offset: offset, size: n}

  defp lay_out_field({:bits, name, bits}, offset) do
    width = Enum.reduce(bits, 0, fn {_name, w, _enum}, acc -> acc + w end)
    size = div(width + 7, 8)

    cond do
      width <= 0 ->
        raise ArgumentError, "bit group #{inspect(name)} is empty"

      width > 32 ->
        raise ArgumentError,
              "bit group #{inspect(name)} is #{width} bits wide; 32 is the maximum " <>
                "(it becomes one u32le)"

      size * 8 - width >= 8 ->
        raise ArgumentError,
              "bit group #{inspect(name)} is #{width} bits wide, which wastes a whole byte; " <>
                "pad it to #{size * 8} bits and name the padding"

      true ->
        :ok
    end

    {decoded, _} =
      Enum.map_reduce(bits, 0, fn {bit_name, bit_width, enum}, bit_offset ->
        bit = %{name: bit_name, width: bit_width, offset: bit_offset}
        bit = if enum, do: Map.put(bit, :enum, enum), else: bit
        {bit, bit_offset + bit_width}
      end)

    %{kind: :bits, name: name, offset: offset, size: size, bits: decoded}
  end

  # Generates {pattern_elements, prelude_statements, result_pairs}.
  defp codegen(fields) do
    Enum.reduce(fields, {[], [], []}, fn field, {pattern, prelude, pairs} ->
      case field.kind do
        :u8 ->
          v = var(field.name)
          {pattern ++ [v], prelude, pairs ++ [{field.name, v}]}

        :u16le ->
          v = var(field.name)
          {pattern ++ [quote(do: unquote(v) :: little - 16)], prelude, pairs ++ [{field.name, v}]}

        :i16le ->
          v = var(field.name)

          {pattern ++ [quote(do: unquote(v) :: little - signed - 16)], prelude, pairs ++ [{field.name, v}]}

        :u32le ->
          v = var(field.name)
          {pattern ++ [quote(do: unquote(v) :: little - 32)], prelude, pairs ++ [{field.name, v}]}

        :bytes ->
          v = var(field.name)

          {pattern ++ [quote(do: unquote(v) :: binary - size(unquote(field.size)))], prelude,
           pairs ++ [{field.name, v}]}

        :skip ->
          {pattern ++ [quote(do: _ :: binary - size(unquote(field.size)))], prelude, pairs}

        :bits ->
          raw = Macro.var(:"padwave_raw_#{field.name}", nil)
          map = var(field.name)

          bit_pairs = Enum.map(field.bits, fn bit -> {bit.name, bit_expr(raw, bit)} end)

          assignment = quote(do: unquote(map) = %{unquote_splicing(bit_pairs)})

          {pattern ++ [quote(do: unquote(raw) :: unquote(bits_integer(field.size)))], prelude ++ [assignment],
           pairs ++ [{field.name, map}]}
      end
    end)
  end

  # Type spec for a merged bit group: a plain bit count for one byte, and the
  # little-endian type spec AST for wider ones (unquoted into the pattern).
  defp bits_integer(1), do: 8
  defp bits_integer(n) when n in 2..4, do: {:-, [], [{:little, [], Elixir}, n * 8]}

  defp bit_expr(raw, %{enum: enum, width: width, offset: offset}) do
    mask = bsl(1, width) - 1
    value = quote(do: unquote(raw) >>> unquote(offset) &&& unquote(mask))

    # The spec keeps the enum as declared (name => value); the decoder needs
    # the reverse lookup.
    lookup = Map.new(enum, fn {name, number} -> {number, name} end)
    quote(do: Map.get(unquote(Macro.escape(lookup)), unquote(value), unquote(value)))
  end

  defp bit_expr(raw, %{width: 1, offset: offset}) do
    quote(do: (unquote(raw) >>> unquote(offset) &&& 1) == 1)
  end

  defp bit_expr(raw, %{width: width, offset: offset}) do
    mask = bsl(1, width) - 1
    quote(do: unquote(raw) >>> unquote(offset) &&& unquote(mask))
  end

  defp var(name), do: Macro.var(name, nil)
end
