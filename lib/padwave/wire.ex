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
          u8   :seq
          u8   :left_x
          bits :buttons, dpad: 4, square: 1, cross: 1, circle: 1, triangle: 1
          i16  :temperature
        end
      end

      MySchema.decode_input(<<0x31, 1, 127, 0x48, 0x01, 0x00>>)
      #=> %{seq: 1, left_x: 127, buttons: %{dpad: 8, square: false, ...}, temperature: 1}

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
      import Padwave.Wire, only: [packet: 3, packet: 4]
      import Bitwise
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
  defp fields({:__block__, _, statements}, env), do: Enum.flat_map(statements, &fields(&1, env))

  defp fields({:u8, _, [name]}, _env) when is_atom(name), do: [{:u8, name}]
  defp fields({:u16, _, [name]}, _env) when is_atom(name), do: [{:u16, name}]
  defp fields({:i16, _, [name]}, _env) when is_atom(name), do: [{:i16, name}]
  defp fields({:u32, _, [name]}, _env) when is_atom(name), do: [{:u32, name}]

  defp fields({:bytes, _, [name, count]}, _env) when is_atom(name) and is_integer(count) do
    [{:bytes, name, count}]
  end

  defp fields({:skip, _, [count]}, _env) when is_integer(count), do: [{:skip, count}]

  defp fields({:bits, _, [name, bits]}, _env) when is_atom(name) and is_list(bits) do
    [{:bits, name, bits}]
  end

  defp fields(other, _env) do
    raise ArgumentError, """
    cannot understand packet field: #{Macro.to_string(other)}

    Supported fields are:
      u8 :name  u16 :name  i16 :name  u32 :name
      bytes :name, count
      skip count
      bits :name, low: 4, high: 1, ...
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
  defp lay_out_field({:u16, name}, offset), do: %{kind: :u16, name: name, offset: offset, size: 2}
  defp lay_out_field({:i16, name}, offset), do: %{kind: :i16, name: name, offset: offset, size: 2}
  defp lay_out_field({:u32, name}, offset), do: %{kind: :u32, name: name, offset: offset, size: 4}

  defp lay_out_field({:bytes, name, n}, offset),
    do: %{kind: :bytes, name: name, offset: offset, size: n}

  defp lay_out_field({:skip, n}, offset), do: %{kind: :skip, name: nil, offset: offset, size: n}

  defp lay_out_field({:bits, name, bits}, offset) do
    width = Enum.reduce(bits, 0, fn {_name, w}, acc -> acc + w end)

    if width != 8 do
      raise ArgumentError,
            "bit group #{inspect(name)} is #{width} bits wide; only whole bytes are supported"
    end

    {decoded, _} =
      Enum.map_reduce(bits, 0, fn {bit_name, bit_width}, bit_offset ->
        {%{name: bit_name, width: bit_width, offset: bit_offset}, bit_offset + bit_width}
      end)

    %{kind: :bits, name: name, offset: offset, size: 1, bits: decoded}
  end

  # Generates {pattern_elements, prelude_statements, result_pairs}.
  defp codegen(fields) do
    Enum.reduce(fields, {[], [], []}, fn field, {pattern, prelude, pairs} ->
      case field.kind do
        :u8 ->
          v = var(field.name)
          {pattern ++ [v], prelude, pairs ++ [{field.name, v}]}

        :u16 ->
          v = var(field.name)
          {pattern ++ [quote(do: unquote(v) :: little - 16)], prelude, pairs ++ [{field.name, v}]}

        :i16 ->
          v = var(field.name)

          {pattern ++ [quote(do: unquote(v) :: little - signed - 16)], prelude,
           pairs ++ [{field.name, v}]}

        :u32 ->
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

          {pattern ++ [quote(do: unquote(raw) :: 8)], prelude ++ [assignment],
           pairs ++ [{field.name, map}]}
      end
    end)
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
