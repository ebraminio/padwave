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
    assert fields.buttons.dpad == :none
    refute fields.buttons.cross
    refute fields.buttons.triangle

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
    assert {:error, {:unexpected_report, 5, _}} = DualSenseInput.decode_input(<<0x31, 1, 2, 3, 4>>)
  end

  test "rejects a corrupted Bluetooth report instead of inventing a button", %{raw: raw} do
    <<head::binary-size(10), byte, tail::binary>> = raw
    corrupted = <<head::binary, Bitwise.bxor(byte, 0x40), tail::binary>>

    assert {:error, :bad_crc} = Frame.from_report(corrupted)
    assert {:ok, _frame} = Frame.from_report(raw)
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
    assert offsets.buttons == 9
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

  test "a USB report decodes through the same body" do
    usb =
      <<0x01, 80, 90, 100, 110, 0, 0, 7, 8, 0, 0, 0, 0>> <>
        :binary.copy(<<0>>, 51)

    assert byte_size(usb) == 64
    assert {:error, _} = Frame.from_report(usb)

    assert {:ok, frame} = Frame.from_report(usb, :input_usb)
    assert frame.lx == 80
    assert frame.ly == 90
    assert frame.rx == 100
    assert frame.ry == 110
    assert frame.seq == 7
    assert frame.dpad == :none
  end

  test "USB is the Bluetooth layout minus the header" do
    bt = Map.new(DualSenseInput.spec(:input).fields, &{&1.name, &1.offset})
    usb = Map.new(DualSenseInput.spec(:input_usb).fields, &{&1.name, &1.offset})

    assert DualSenseInput.spec(:input_usb).id == 0x01
    assert DualSenseInput.spec(:input_usb).size == 64

    for name <- [:lx, :seq, :gyro_x, :accel_x, :temperature, :touch1, :status0] do
      assert usb[name] == bt[name] - 1
    end
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
