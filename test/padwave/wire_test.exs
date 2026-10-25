defmodule Padwave.WireTest do
  use ExUnit.Case, async: true

  alias Padwave.Input.Frame
  alias Padwave.Schema.DualSenseInput

  # Captured from a real DualSense over Bluetooth:
  #   sudo cat /dev/hidraw0 | xxd
  @captured "31d17f807f83000001080000001ef99bb5fbfffcffffff0600dc1ebc058c0bb1530e86e661088408a3039309090000000000b516b153010700f5d6b13c03620f40000000000000000000b4449c94"

  setup do
    {:ok, raw: Base.decode16!(@captured, case: :mixed)}
  end

  test "decodes the captured report", %{raw: raw} do
    fields = DualSenseInput.decode_input(raw)

    assert fields.lx == 0x7F
    assert fields.ly == 0x80
    assert fields.rx == 0x7F
    assert fields.ry == 0x83
    assert fields.l2 == 0
    assert fields.r2 == 0
    assert fields.seq == 1

    # d-pad neutral is 8, no face buttons pressed
    assert fields.buttons0.dpad == 8
    refute fields.buttons0.cross
    refute fields.buttons0.triangle

    # status0 = 0x01 -> level 1 of 0..10, discharging; the kernel's
    # power_supply node simultaneously reported capacity=15.
    assert fields.status0 == 0x01
    # headphones were plugged in
    assert Bitwise.band(fields.status1, 1) == 1
  end

  test "the sensor block is little-endian and lands where the accelerometer says 1g",
       %{raw: raw} do
    fields = DualSenseInput.decode_input(raw)

    magnitude =
      :math.sqrt(
        fields.accel_x * fields.accel_x +
          fields.accel_y * fields.accel_y +
          fields.accel_z * fields.accel_z
      )

    # resting accelerometer reads roughly 1g (~8192 counts)
    assert magnitude > 7000 and magnitude < 9000
  end

  test "rejects packets of the wrong size" do
    assert {:error, {:unexpected_report, 5, _}} =
             DualSenseInput.decode_input(<<0x31, 1, 2, 3, 4>>)
  end

  test "rejects a different report id" do
    assert {:error, _} = DualSenseInput.decode_input(<<0x01, 0::size(77 * 8)>>)
  end

  test "the introspectable spec agrees with the decoder" do
    spec = DualSenseInput.spec(:input)

    assert spec.id == 0x31
    assert spec.size == 78

    offsets = Map.new(spec.fields, fn field -> {field.name, field.offset} end)

    assert offsets.lx == 2
    assert offsets.buttons0 == 9
    assert offsets.gyro_x == 17
    assert offsets.accel_x == 23
    assert offsets.status0 == 54
    assert offsets.crc == 74
  end

  test "g-force reads about 1 g at rest and touch points decode", %{raw: raw} do
    {:ok, frame} = Frame.from_report(raw)

    assert frame.g_force > 0.5 and frame.g_force < 1.5
    assert length(frame.touches) == 2

    # in this capture no finger is on the pad: bit 7 of the contact byte is set
    assert Enum.all?(frame.touches, &(&1.active == false))
  end

  test "a touch point unpacks its nibbles the way the kernel does" do
    # contact 0x05 = finger 5 down; x_lo 0x34; 0x21 packs x_hi=1 (low nibble)
    # and y_lo=2 (high nibble); y_hi 0x06. Same packing as the kernel's
    # `u8 x_hi:4, y_lo:4` bitfield.
    assert %{active: true, id: 5, x: 0x134, y: 0x62} = Frame.touch(<<0x05, 0x34, 0x21, 0x06>>)
  end

  test "frames expose named buttons, battery and jack state", %{raw: raw} do
    {:ok, frame} = Frame.from_report(raw)

    assert frame.buttons == []
    assert frame.dpad == :none
    assert frame.battery == 15
    assert frame.charging == :discharging
    assert frame.jack == true
  end
end
