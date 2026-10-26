defmodule Padwave.Schema.DualSenseInput.Body do
  @moduledoc """
  The 63-byte input payload shared by the USB and Bluetooth reports.

  It lives in its own module so it is compiled before the packets that splice
  it in: Elixir applies module-attribute writes when it evaluates the body,
  after macro expansion, so a field list declared in the same module as the
  packets would still be empty when they are expanded.

  `+0x00` in the comments is the offset inside this payload: 0x01 in a USB
  report, 0x02 in a Bluetooth one.

  When microphone duplex is on (see `Padwave.Schema.DualSenseAudio`), not every
  `0x31` frame is a gamepad report: the controller interleaves audio frames
  that keep the report id but put the Opus TOC `0xD4` at report byte 3
  (payload `+0x01`, i.e. where `ly` normally is). Demux on that byte before
  trusting any of these fields.
  """

  use Padwave.Wire

  field_list :input_body do
    u8 :lx                       # +0x00  left stick X, 0..255 (128 = centre)
    u8 :ly                       # +0x01  left stick Y, 0 = up
    u8 :rx                       # +0x02  right stick X
    u8 :ry                       # +0x03  right stick Y
    u8 :l2                       # +0x04  L2 analogue
    u8 :r2                       # +0x05  R2 analogue
    u8 :seq                      # +0x06  report sequence number

    # Bit groups read the low bit first: d-pad in the low nibble, face buttons
    # above it.
    bits :buttons0, dpad: 4, square: 1, cross: 1, circle: 1, triangle: 1
    bits :buttons1, l1: 1, r1: 1, l2: 1, r2: 1, create: 1, options: 1, l3: 1, r3: 1

    bits :buttons2,
      ps: 1,                     # +0x09 bit 0  PS / home
      touchpad: 1,               #       bit 1  touchpad click
      mute: 1,                   #       bit 2  microphone mute
      reserved_0: 2,             #       bits 3-4
      reserved_1: 1,             #       bit 5
      left_fn: 1,                #       bit 6  Edge only
      right_fn: 1                #       bit 7  Edge only

    bits :buttons3, reserved_0: 4, left_paddle: 1, right_paddle: 1, reserved_1: 2
    # +0x0a  Edge paddles; zero on a standard DualSense

    u8 :incremental              # +0x0b  increments every report, wraps at 256
    bytes :reserved_14, 3        # +0x0c..0x0e  not documented

    i16le :gyro_x                # +0x0f  angular velocity, little-endian
    i16le :gyro_y                # +0x11
    i16le :gyro_z                # +0x13
    i16le :accel_x               # +0x15  acceleration, ~8192 counts per g
    i16le :accel_y               # +0x17
    i16le :accel_z               # +0x19
    u32le :sensor_time           # +0x1b  timestamp, 0.33 us units
    u8 :temperature              # +0x1f  controller temperature, degrees C

    # Four bytes per touch point: contact/id, x low, (x high | y low), y high.
    # Bit 7 of the contact byte means "not touching".
    bytes :touch1, 4             # +0x20
    bytes :touch2, 4             # +0x24

    u8 :trigger_feedback_l       # +0x28  L2 adaptive-trigger status
    u8 :trigger_feedback_r       # +0x29
    u32le :host_time             # +0x2a  host timestamp echoed back
    bytes :device_feedback, 4    # +0x2e  device timestamp / trigger feedback
    bytes :reserved_52, 2        # +0x32

    u8 :status0                  # +0x34  battery (low nibble) + charging (high)
    u8 :status1,
       flags: [headset: 0, microphone: 1]
    # +0x35  what is plugged into the 3.5 mm jack
    u8 :status2                  # +0x36
    bytes :aes_cmac, 8           # +0x37  pairing CMAC
  end
end

defmodule Padwave.Schema.DualSenseInput do
  @moduledoc """
  DualSense input reports, USB and Bluetooth.

  * `:input` — Bluetooth, report `0x31`, 78 bytes, CRC at the end.
  * `:input_usb` — USB, report `0x01`, 64 bytes, no CRC.

  Both carry the same 63-byte payload, described once in
  `Padwave.Schema.DualSenseInput.Body` and spliced into both packets. Only the
  framing differs: the Bluetooth report has a sequence tag in front, a
  nine-byte tail and a CRC behind. That is why the kernel parses both with a
  single `struct dualsense_input_report`, pointing at `&data[1]` (USB) or
  `&data[2]` (Bluetooth).

  Every byte is named, either with what it does or with an honest
  `reserved`/`feedback` name, and every multi-byte integer states its byte
  order (`i16le`, `u32le`). `test/padwave/wire_test.exs` asserts that every
  shared field's USB offset is exactly its Bluetooth offset minus one.

  Sizes and report ids come from `hid-playstation.c`:
  `DS_INPUT_REPORT_USB_SIZE` 64, `DS_INPUT_REPORT_BT_SIZE` 78.
  """

  use Padwave.Wire

  packet :input, 0x31, size: 78, doc: "DualSense input report (Bluetooth)" do
    u8 :seq_tag                  # 0x01  high nibble = sequence tag, low nibble = 0
    splice Padwave.Schema.DualSenseInput.Body, :input_body

    bytes :bt_reserved, 9        # 0x41..0x49  Bluetooth-only, not documented
    u32le :crc                   # 0x4a  CRC-32 over the report, seed 0xA1
  end

  packet :input_usb, 0x01, size: 64, doc: "DualSense input report (USB)" do
    splice Padwave.Schema.DualSenseInput.Body, :input_body
  end
end
