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
        owned_fields: 0x0400,
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
    report = Report.lightbar(10, 20, 30, seq: 3)
    <<0x31, seq_tag, tag, flag0, flag1, _rest::binary>> = report

    assert seq_tag == 3 <<< 4
    assert tag == 0x10
    assert flag0 == 0
    assert flag1 == 0x04
    assert byte_size(report) == 78
  end

  test "USB output reports are 63 bytes, id 0x02, no CRC" do
    report = Report.lightbar(1, 2, 3, bus: :usb)

    assert byte_size(report) == 63
    assert <<0x02, 0x00, 0x04, rest::binary>> = report
    assert byte_size(rest) == 60
  end

  test "the USB spec is the shared payload behind a one-byte header" do
    bt = Map.new(DualSenseOutput.spec(:output).fields, &{&1.name, &1.offset})
    usb = Map.new(DualSenseOutput.spec(:output_usb).fields, &{&1.name, &1.offset})

    assert DualSenseOutput.spec(:output_usb).id == 0x02
    assert DualSenseOutput.spec(:output_usb).size == 63

    for name <- [:owned_fields, :lightbar_red, :player_leds, :lightbar_blue] do
      assert usb[name] == bt[name] - 2
    end
  end

  test "the two flag bytes are one 16-bit mask with named bits" do
    spec = Padwave.Schema.DualSenseOutput.spec(:output)
    owned = Enum.find(spec.fields, &(&1.name == :owned_fields))

    assert owned.size == 2
    assert owned.flags[:right_trigger] == 2
    assert owned.flags[:audio_routing] == 7
    assert owned.flags[:lightbar] == 10
    assert owned.flags[:audio_control2] == 15

    report = Report.build(%{owned_fields: [:right_trigger, :left_trigger]})
    assert Padwave.Schema.DualSenseOutput.decode_output(report).owned_fields == 0x0C

    report = Report.build(%{owned_fields: [:lightbar, :player_leds]})
    assert Padwave.Schema.DualSenseOutput.decode_output(report).owned_fields == 0x1400

    assert_raise ArgumentError, ~r/unknown flag bit/, fn ->
      Report.build(%{owned_fields: [:nonsense]})
    end
  end

  test "a trigger vibration sets its flag, mode and parameters" do
    report = Report.trigger(:right, 0x06, [25, 200, 5], seq: 7)

    assert byte_size(report) == 78
    assert <<0x31, seq_tag, 0x10, _rest::binary>> = report
    assert seq_tag == 7 <<< 4

    fields = Padwave.Schema.DualSenseOutput.decode_output(report)
    assert fields.owned_fields == 0x04
    assert fields.right_trigger_mode == 0x06
    assert fields.right_trigger_params == <<25, 200, 5, 0, 0, 0, 0, 0, 0, 0>>
    assert fields.left_trigger_mode == 0x00

    # and the report is signed
    <<body::binary-size(74), crc::little-32>> = report
    assert Report.crc(body) == crc
  end

  test "the left trigger uses its own flag and parameter block" do
    report = Report.trigger(:left, 0x00, [])
    fields = Padwave.Schema.DualSenseOutput.decode_output(report)

    assert fields.owned_fields == 0x08
    assert fields.left_trigger_mode == 0x00
    assert fields.right_trigger_mode == 0x00
  end

  test "the output spec describes the same bytes as the encoder" do
    spec = DualSenseOutput.spec(:output)
    offsets = Map.new(spec.fields, fn field -> {field.name, field.offset} end)

    assert spec.size == 78
    assert offsets.owned_fields == 3
    assert offsets.lightbar_red == 47
    assert offsets.lightbar_green == 48
    assert offsets.lightbar_blue == 49
    assert offsets.crc == 74
  end
end
