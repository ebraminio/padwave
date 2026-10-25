defmodule Padwave.Schema.DualSenseInput do
  @moduledoc """
  DualSense input report `0x31`, Bluetooth flavour (78 bytes).

  This file is the *description* of the controller's input packet. It is the
  thing you edit when the layout needs to change — and it can be reloaded into
  the running service with `Padwave.HotSwap.reload/0` or `POST /api/reload`
  without dropping the music.

  Offsets below were verified against a live controller over Bluetooth:

    0x00  report id (0x31)
    0x01  sequence/flags (0xD1 on BT: high nibble = sequence tag)
    0x02  left stick X      0x03 left stick Y
    0x04  right stick X     0x05 right stick Y
    0x06  L2 analogue       0x07 R2 analogue
    0x08  report sequence number
    0x09  buttons 0 (d-pad low nibble, face buttons high nibble)
    0x0a  buttons 1 (L1 R1 L2 R2 create options L3 R3)
    0x0b  buttons 2 (PS, touchpad, mute, ...)
    0x0e  packet counter
    0x11..0x16  gyroscope   (3 x int16 LE)
    0x17..0x1c  accelerometer (3 x int16 LE)
    0x1d  sensor timestamp  (uint32 LE)
    0x21  temperature
    0x22  touch points      (8 bytes, two 4-byte points)
    0x36  status 0 — battery level (low nibble) + charging state (high nibble)
    0x37  status 1 — bit 0 headset plugged, bit 1 microphone
    0x38  status 2
    0x39  AES-CMAC (8 bytes)
    0x4a  CRC-32 (4 bytes, little endian)

  The status byte decoding is corroborated by the kernel: with the captured
  report above the controller reported `status0 = 0x01`, and
  `/sys/class/power_supply/ps-controller-battery-*` simultaneously read
  `capacity = 15`, i.e. level 1 of 0..10.
  """

  use Padwave.Wire

  packet :input, 0x31, size: 78, doc: "DualSense input report (Bluetooth)" do
    u8(:seq_tag)
    u8(:lx)
    u8(:ly)
    u8(:rx)
    u8(:ry)
    u8(:l2)
    u8(:r2)
    u8(:seq)

    bits(:buttons0, dpad: 4, square: 1, cross: 1, circle: 1, triangle: 1)
    bits(:buttons1, l1: 1, r1: 1, l2: 1, r2: 1, create: 1, options: 1, l3: 1, r3: 1)
    bits(:buttons2, ps: 1, touchpad: 1, mute: 1, reserved: 3, left_fn: 1, right_fn: 1)

    skip(2)
    u8(:counter)
    skip(2)

    i16(:gyro_x)
    i16(:gyro_y)
    i16(:gyro_z)
    i16(:accel_x)
    i16(:accel_y)
    i16(:accel_z)
    u32(:sensor_time)
    u8(:temperature)
    bytes(:touch, 8)

    # trigger feedback / host timestamps / device timestamps — not needed yet,
    # but kept as an explicit gap so the offsets below stay honest.
    skip(12)

    u8(:status0)
    u8(:status1)
    u8(:status2)
    bytes(:aes_cmac, 8)
    skip(9)
    u32(:crc)
  end
end
