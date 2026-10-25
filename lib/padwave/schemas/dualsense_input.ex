defmodule Padwave.Schema.DualSenseInput do
  @moduledoc """
  DualSense input report `0x31`, Bluetooth flavour (78 bytes).

  This file is the *description* of the controller's input packet, and it is
  meant to be complete: every byte is named, either with what it does or with
  an honest `reserved`/`feedback` name. Nothing is skipped, so anything the
  controller reports can be picked up later without re-deriving offsets.

  Layout follows `struct dualsense_input_report` in the kernel's
  `drivers/hid/hid-playstation.c` (which starts at byte 2 of a Bluetooth
  report, after the report id and sequence tag), plus the community names for
  the adaptive-trigger feedback and timestamp bytes that the kernel does not
  decode.

    0x00  report id (0x31)
    0x01  sequence/flags (high nibble = sequence tag)
    0x02  left stick X        0x03 left stick Y
    0x04  right stick X       0x05 right stick Y
    0x06  L2 analogue         0x07 R2 analogue
    0x08  report sequence number
    0x09  buttons 0 — d-pad (low nibble) + square/cross/circle/triangle
    0x0a  buttons 1 — L1 R1 L2 R2 create options L3 R3
    0x0b  buttons 2 — PS, touchpad, mute, Fn buttons (Edge)
    0x0c  buttons 3 — paddles (Edge)
    0x0d  incremental report counter
    0x0e..0x10  reserved
    0x11..0x16  gyroscope (3 × int16 LE)
    0x17..0x1c  accelerometer (3 × int16 LE, ~8192 counts per g)
    0x1d..0x20  sensor timestamp (uint32 LE, 0.33 µs units)
    0x21  temperature
    0x22..0x25  touch point 1 (contact, x_lo, x_hi|y_lo, y_hi)
    0x26..0x29  touch point 2
    0x2a  L2 adaptive-trigger feedback
    0x2b  R2 adaptive-trigger feedback
    0x2c..0x2f  host timestamp
    0x30..0x33  device timestamp / trigger feedback
    0x34..0x35  reserved
    0x36  status 0 — battery level (low nibble) + charging state (high nibble)
    0x37  status 1 — bit 0 headset plugged, bit 1 microphone
    0x38  status 2
    0x39..0x40  AES-CMAC
    0x41..0x49  Bluetooth-only reserved
    0x4a  CRC-32 (4 bytes, little endian)
  """

  use Padwave.Wire

  packet :input, 0x31, size: 78, doc: "DualSense input report (Bluetooth)" do
    # -- header ----------------------------------------------------------
    # 0x01  high nibble = sequence tag, low nibble = 0
    u8(:seq_tag)
    # 0x02  left stick X, 0..255 (128 = centre)
    u8(:lx)
    # 0x03  left stick Y, 0 = up
    u8(:ly)
    # 0x04  right stick X
    u8(:rx)
    # 0x05  right stick Y
    u8(:ry)
    # 0x06  L2 analogue, 0..255
    u8(:l2)
    # 0x07  R2 analogue, 0..255
    u8(:r2)
    # 0x08  report sequence number
    u8(:seq)

    # -- buttons ---------------------------------------------------------
    # Bit groups are little-endian: the first name is the lowest bit.
    bits(:buttons0, dpad: 4, square: 1, cross: 1, circle: 1, triangle: 1)
    # 0x09  d-pad 0..7 clockwise from up, 8 = centred; then the face buttons

    bits(:buttons1, l1: 1, r1: 1, l2: 1, r2: 1, create: 1, options: 1, l3: 1, r3: 1)
    # 0x0a  shoulders, triggers at full travel, create/options, stick clicks

    bits(:buttons2,
      # 0x0b bit 0  PS / home
      ps: 1,
      #       bit 1  touchpad click
      touchpad: 1,
      #       bit 2  microphone mute button
      mute: 1,
      #       bits 3-4
      reserved_0: 2,
      #       bit 5
      reserved_1: 1,
      #       bit 6  DualSense Edge only
      left_fn: 1,
      #       bit 7  DualSense Edge only
      right_fn: 1
    )

    bits(:buttons3, reserved_0: 4, left_paddle: 1, right_paddle: 1, reserved_1: 2)
    # 0x0c  Edge paddles; all zero on a standard DualSense

    # -- report bookkeeping ----------------------------------------------
    # 0x0d  increments every report, wraps at 256
    u8(:incremental)
    # 0x0e..0x10  not documented
    bytes(:reserved_14, 3)

    # -- motion ----------------------------------------------------------
    # 0x11  angular velocity, little-endian
    i16(:gyro_x)
    # 0x13
    i16(:gyro_y)
    # 0x15
    i16(:gyro_z)
    # 0x17  acceleration, ~8192 counts per g
    i16(:accel_x)
    # 0x19
    i16(:accel_y)
    # 0x1b
    i16(:accel_z)
    # 0x1d  sensor timestamp, 0.33 us units
    u32(:sensor_time)
    # 0x21  controller temperature, degrees C
    u8(:temperature)

    # -- touchpad --------------------------------------------------------
    # Four bytes per point: contact/id, x low, (x high | y low), y high.
    # Bit 7 of the contact byte means "not touching".
    # 0x22
    bytes(:touch1, 4)
    # 0x26
    bytes(:touch2, 4)

    # -- adaptive trigger feedback ---------------------------------------
    # 0x2a  L2 trigger status as the pad reports it
    u8(:trigger_feedback_l)
    # 0x2b
    u8(:trigger_feedback_r)
    # 0x2c  host timestamp echoed back
    u32(:host_time)
    # 0x30..0x33  device timestamp / trigger feedback
    bytes(:device_feedback, 4)
    # 0x34..0x35
    bytes(:reserved_52, 2)

    # -- status ----------------------------------------------------------
    # 0x36  battery level (low nibble) + charging (high)
    u8(:status0)
    # 0x37  bit 0 headset, bit 1 microphone
    u8(:status1)
    # 0x38
    u8(:status2)

    # 0x39..0x40  pairing CMAC
    bytes(:aes_cmac, 8)
    # 0x41..0x49  Bluetooth-only, not documented
    bytes(:bt_reserved, 9)
    # 0x4a  CRC-32 of the report, seed 0xA1
    u32(:crc)
  end
end
