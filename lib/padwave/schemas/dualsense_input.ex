defmodule Padwave.Schema.DualSenseInput do
  @moduledoc """
  DualSense input reports, USB and Bluetooth.

  * `:input` — Bluetooth, report `0x31`, 78 bytes, CRC at the end.
  * `:input_usb` — USB, report `0x01`, 64 bytes, no CRC.

  The two share a 63-byte payload and differ only in framing: Bluetooth adds a
  sequence tag in front, a nine-byte tail and a CRC behind. That is why the
  kernel parses both with a single `struct dualsense_input_report`, pointing at
  `&data[1]` for USB and `&data[2]` for Bluetooth.

  The payload is written out in full in both packets, in order, with a comment
  per field: `+0x00` in the comments is the offset inside that payload, so both
  copies read identically. Duplication is deliberate — it keeps each packet
  readable top-to-bottom — and `test/padwave/wire_test.exs` asserts every shared
  field's USB offset is exactly its Bluetooth offset minus one, so the two
  copies cannot drift apart unnoticed.

  Sizes and report ids come from `hid-playstation.c`
  (`DS_INPUT_REPORT_USB_SIZE` 64, `DS_INPUT_REPORT_BT_SIZE` 78).
  """

  use Padwave.Wire

  packet :input, 0x31, size: 78, doc: "DualSense input report (Bluetooth)" do
    # 0x01  high nibble = sequence tag, low nibble = 0
    u8(:seq_tag)

    # -- payload starts at 0x02 ------------------------------------------
    # +0x00  left stick X, 0..255 (128 = centre)
    u8(:lx)
    # +0x01  left stick Y, 0 = up
    u8(:ly)
    # +0x02  right stick X
    u8(:rx)
    # +0x03  right stick Y
    u8(:ry)
    # +0x04  L2 analogue
    u8(:l2)
    # +0x05  R2 analogue
    u8(:r2)
    # +0x06  report sequence number
    u8(:seq)

    # Bit groups read low bit first: d-pad in the low nibble, face buttons
    # above it.
    bits(:buttons0, dpad: 4, square: 1, cross: 1, circle: 1, triangle: 1)
    bits(:buttons1, l1: 1, r1: 1, l2: 1, r2: 1, create: 1, options: 1, l3: 1, r3: 1)

    bits(:buttons2,
      # +0x09 bit 0  PS / home
      ps: 1,
      #       bit 1  touchpad click
      touchpad: 1,
      #       bit 2  microphone mute
      mute: 1,
      #       bits 3-4
      reserved_0: 2,
      #       bit 5
      reserved_1: 1,
      #       bit 6  Edge only
      left_fn: 1,
      #       bit 7  Edge only
      right_fn: 1
    )

    bits(:buttons3, reserved_0: 4, left_paddle: 1, right_paddle: 1, reserved_1: 2)
    # +0x0a  Edge paddles; zero on a standard DualSense

    # +0x0b  increments every report, wraps at 256
    u8(:incremental)
    # +0x0c..0x0e  not documented
    bytes(:reserved_14, 3)

    # +0x0f  angular velocity, little-endian
    i16(:gyro_x)
    # +0x11
    i16(:gyro_y)
    # +0x13
    i16(:gyro_z)
    # +0x15  acceleration, ~8192 counts per g
    i16(:accel_x)
    # +0x17
    i16(:accel_y)
    # +0x19
    i16(:accel_z)
    # +0x1b  timestamp, 0.33 us units
    u32(:sensor_time)
    # +0x1f  controller temperature, degrees C
    u8(:temperature)

    # Four bytes per touch point: contact/id, x low, (x high | y low), y high.
    # Bit 7 of the contact byte means "not touching".
    # +0x20
    bytes(:touch1, 4)
    # +0x24
    bytes(:touch2, 4)

    # +0x28  L2 adaptive-trigger status
    u8(:trigger_feedback_l)
    # +0x29
    u8(:trigger_feedback_r)
    # +0x2a  host timestamp echoed back
    u32(:host_time)
    # +0x2e  device timestamp / trigger feedback
    bytes(:device_feedback, 4)
    # +0x32
    bytes(:reserved_52, 2)

    # +0x34  battery (low nibble) + charging (high)
    u8(:status0)
    # +0x35  bit 0 headset, bit 1 microphone
    u8(:status1)
    # +0x36
    u8(:status2)
    # +0x37  pairing CMAC
    bytes(:aes_cmac, 8)
    # -- end of payload ---------------------------------------------------

    # 0x41..0x49  Bluetooth-only, not documented
    bytes(:bt_reserved, 9)
    # 0x4a  CRC-32 over the report, seed 0xA1
    u32(:crc)
  end

  packet :input_usb, 0x01, size: 64, doc: "DualSense input report (USB)" do
    # -- payload starts at 0x01 ------------------------------------------
    # +0x00  left stick X, 0..255 (128 = centre)
    u8(:lx)
    # +0x01  left stick Y, 0 = up
    u8(:ly)
    # +0x02  right stick X
    u8(:rx)
    # +0x03  right stick Y
    u8(:ry)
    # +0x04  L2 analogue
    u8(:l2)
    # +0x05  R2 analogue
    u8(:r2)
    # +0x06  report sequence number
    u8(:seq)

    bits(:buttons0, dpad: 4, square: 1, cross: 1, circle: 1, triangle: 1)
    bits(:buttons1, l1: 1, r1: 1, l2: 1, r2: 1, create: 1, options: 1, l3: 1, r3: 1)

    bits(:buttons2,
      # +0x09 bit 0  PS / home
      ps: 1,
      #       bit 1  touchpad click
      touchpad: 1,
      #       bit 2  microphone mute
      mute: 1,
      #       bits 3-4
      reserved_0: 2,
      #       bit 5
      reserved_1: 1,
      #       bit 6  Edge only
      left_fn: 1,
      #       bit 7  Edge only
      right_fn: 1
    )

    bits(:buttons3, reserved_0: 4, left_paddle: 1, right_paddle: 1, reserved_1: 2)
    # +0x0a  Edge paddles; zero on a standard DualSense

    # +0x0b  increments every report, wraps at 256
    u8(:incremental)
    # +0x0c..0x0e  not documented
    bytes(:reserved_14, 3)

    # +0x0f  angular velocity, little-endian
    i16(:gyro_x)
    # +0x11
    i16(:gyro_y)
    # +0x13
    i16(:gyro_z)
    # +0x15  acceleration, ~8192 counts per g
    i16(:accel_x)
    # +0x17
    i16(:accel_y)
    # +0x19
    i16(:accel_z)
    # +0x1b  timestamp, 0.33 us units
    u32(:sensor_time)
    # +0x1f  controller temperature, degrees C
    u8(:temperature)

    # +0x20  contact/id, x low, x high | y low, y high
    bytes(:touch1, 4)
    # +0x24
    bytes(:touch2, 4)
    # +0x28  L2 adaptive-trigger status
    u8(:trigger_feedback_l)
    # +0x29
    u8(:trigger_feedback_r)
    # +0x2a  host timestamp echoed back
    u32(:host_time)
    # +0x2e  device timestamp / trigger feedback
    bytes(:device_feedback, 4)
    # +0x32
    bytes(:reserved_52, 2)

    # +0x34  battery (low nibble) + charging (high)
    u8(:status0)
    # +0x35  bit 0 headset, bit 1 microphone
    u8(:status1)
    # +0x36
    u8(:status2)
    # +0x37  pairing CMAC
    bytes(:aes_cmac, 8)
  end
end
