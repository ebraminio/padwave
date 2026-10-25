defmodule Padwave.Input.Crc do
  @moduledoc """
  The DualSense signs its Bluetooth reports.

  Input reports carry a CRC-32 in their last four bytes, seeded with `0xA1`
  (output reports use `0xA2`). The kernel drops reports whose CRC does not
  match:

      crc = crc32_le(0xFFFFFFFF, &seed, 1);
      crc = ~crc32_le(crc, data, size - 4);

  which is the same as `:erlang.crc32(<<seed>> <> data)`.

  Validating this matters more than it sounds: a corrupted report can flip a
  button bit, and an unvalidated stream turns radio noise into phantom presses
  — a suspend here, a mute there, and an idle timer that never fires because a
  phantom stick "moved".
  """

  @input_seed 0xA1

  @doc "True when the trailing CRC of a Bluetooth input report is correct."
  def valid_input?(report) when is_binary(report) and byte_size(report) > 4 do
    size = byte_size(report)
    data = binary_part(report, 0, size - 4)
    <<_::binary-size(size - 4), stored::little-32>> = report

    :erlang.crc32(<<@input_seed>> <> data) == stored
  end

  def valid_input?(_report), do: false

  @doc """
  Recomputes the trailing CRC in place. Used by the simulator, which edits
  fields of a real report and must re-sign it.
  """
  def seal_input(report) when is_binary(report) and byte_size(report) > 4 do
    size = byte_size(report)
    data = binary_part(report, 0, size - 4)
    crc = :erlang.crc32(<<@input_seed>> <> data)
    <<data::binary, crc::little-32>>
  end

  def seal_input(report), do: report
end
