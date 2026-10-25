defmodule Padwave.Schema.DualSenseInput do
  @moduledoc """
  DualSense input reports, both transports.

  * `:input` — Bluetooth, report `0x31`, 78 bytes, CRC at the end.
  * `:input_usb` — USB, report `0x01`, 64 bytes, no CRC.

  The payload is the same 63 bytes in both, declared once in
  `Padwave.Schema.Common` and spliced in; only the framing differs. That is
  what "ready for wired USB" means here: the decoder, the offsets and the
  derived values are shared, so the app never sees the difference.

  Sizes and report ids come from the kernel (`hid-playstation.c`):
  `DS_INPUT_REPORT_USB_SIZE` 64, `DS_INPUT_REPORT_BT_SIZE` 78.
  """

  use Padwave.Wire

  packet :input, 0x31, size: 78, doc: "DualSense input report (Bluetooth)" do
    # 0x01  high nibble = sequence tag, low nibble = 0
    u8(:seq_tag)
    splice(Padwave.Schema.Common, :input_body)

    # 0x41..0x49  Bluetooth-only, not documented
    bytes(:bt_reserved, 9)
    # 0x4a  CRC-32 over the report, seed 0xA1
    u32(:crc)
  end

  packet :input_usb, 0x01, size: 64, doc: "DualSense input report (USB)" do
    splice(Padwave.Schema.Common, :input_body)
  end
end
