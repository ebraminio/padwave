defmodule Padwave.OutputReportTest do
  use ExUnit.Case, async: true

  import Bitwise

  alias Padwave.Output.Report
  alias Padwave.Schema.DualSenseOutput

  # Captured from the kernel itself: `btmon -w /tmp/hid.snoop` while writing to
  # /sys/class/leds/input21:rgb:indicator/multi_intensity. The kernel built this
  # packet, so reproducing it byte-for-byte proves our layout *and* our CRC.
  @captured "31e0100004000000000000000000000000000000000000000000000000000000000000000000000000000000000000be0000000000000000000000000000000000000000000000000000fc54721b"

  test "the captured packet is the expected length" do
    assert byte_size(Base.decode16!(@captured, case: :mixed)) == 78
  end

  test "our encoder reproduces the kernel's packet, CRC included" do
    report =
      Report.build(%{
        seq_tag: 0xE0,
        tag: 0x10,
        valid_flag1: 0x04,
        lightbar_red: 0xBE
      })

    assert Base.encode16(report, case: :lower) == @captured
  end

  test "the CRC matches the documented kernel formula" do
    raw = Base.decode16!(@captured, case: :mixed)
    body = binary_part(raw, 0, 74)
    <<_::binary-size(74), stored::little-32>> = raw

    assert Report.crc(body) == stored
  end

  test "a lightbar update only sets the lightbar flag" do
    report = Report.lightbar(10, 20, 30, 3)
    <<0x31, seq_tag, tag, flag0, flag1, _rest::binary>> = report

    assert seq_tag == 3 <<< 4
    assert tag == 0x10
    assert flag0 == 0
    assert flag1 == 0x04
    assert byte_size(report) == 78
  end

  test "the output spec describes the same bytes as the encoder" do
    spec = DualSenseOutput.spec(:output)
    offsets = Map.new(spec.fields, fn field -> {field.name, field.offset} end)

    assert spec.size == 78
    assert offsets.valid_flag1 == 4
    assert offsets.lightbar_red == 47
    assert offsets.lightbar_green == 48
    assert offsets.lightbar_blue == 49
    assert offsets.crc == 74
  end
end
