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

    for name <- [:valid_flag1, :lightbar_red, :player_leds, :lightbar_blue] do
      assert usb[name] == bt[name] - 2
    end
  end

  test "the charging pattern alternates the two checkerboards and rests on the middle LED" do
    assert Padwave.Output.ChargingLeds.pattern() == [0b01010, 0b10101]
    assert Padwave.Output.ChargingLeds.home() == 0b00100
  end

  test "the claim bytes keep the community's valid_flag names" do
    spec = Padwave.Schema.DualSenseOutput.spec(:output)
    flag0 = Enum.find(spec.fields, &(&1.name == :valid_flag0))
    flag1 = Enum.find(spec.fields, &(&1.name == :valid_flag1))
    offset = fn field, name -> Enum.find(field.bits, &(&1.name == name)).offset end

    assert offset.(flag0, :right_trigger) == 2
    assert offset.(flag0, :audio_routing) == 7
    assert offset.(flag1, :lightbar) == 2
    assert offset.(flag1, :audio_control2) == 7

    report = Report.build(%{valid_flag0: [:right_trigger, :left_trigger]})
    fields = Padwave.Schema.DualSenseOutput.decode_output(report)
    assert fields.valid_flag0.right_trigger
    assert fields.valid_flag0.left_trigger
    refute fields.valid_flag1.lightbar

    report = Report.build(%{valid_flag1: [:lightbar, :player_leds]})
    fields = Padwave.Schema.DualSenseOutput.decode_output(report)
    assert fields.valid_flag1.lightbar
    assert fields.valid_flag1.player_leds

    assert_raise ArgumentError, ~r/unknown bit/, fn ->
      Report.build(%{valid_flag0: [:nonsense]})
    end
  end

  test "other bit-setting fields carry names too" do
    spec = Padwave.Schema.DualSenseOutput.spec(:output)
    leds = Enum.find(spec.fields, &(&1.name == :player_leds))
    power = Enum.find(spec.fields, &(&1.name == :power_save_control))

    assert Enum.find(leds.bits, &(&1.name == :led3)).offset == 2
    assert Enum.find(power.bits, &(&1.name == :microphone_mute)).offset == 4

    report = Report.build(%{valid_flag1: [:player_leds], player_leds: [:led1, :led3]})
    fields = Padwave.Schema.DualSenseOutput.decode_output(report)

    assert fields.player_leds.led1
    refute fields.player_leds.led2
    assert fields.player_leds.led3
  end


  test "a trigger vibration sets its flag, mode and parameters" do
    report = Report.trigger(:right, 0x06, [25, 200, 5], seq: 7)

    assert byte_size(report) == 78
    assert <<0x31, seq_tag, 0x10, _rest::binary>> = report
    assert seq_tag == 7 <<< 4

    fields = Padwave.Schema.DualSenseOutput.decode_output(report)
    assert fields.valid_flag0.right_trigger
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

    assert fields.valid_flag0.left_trigger
    assert fields.left_trigger_mode == 0x00
    assert fields.right_trigger_mode == 0x00
  end

  test "the output spec describes the same bytes as the encoder" do
    spec = DualSenseOutput.spec(:output)
    offsets = Map.new(spec.fields, fn field -> {field.name, field.offset} end)

    assert spec.size == 78
    assert offsets.valid_flag0 == 3
    assert offsets.valid_flag1 == 4
    assert offsets.lightbar_red == 47
    assert offsets.lightbar_green == 48
    assert offsets.lightbar_blue == 49
    assert offsets.crc == 74
  end
end
