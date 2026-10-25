defmodule Padwave.Schema.DualSenseOutput do
  @moduledoc """
  DualSense output reports, both transports.

  * `:output` — Bluetooth, report `0x31`, 78 bytes, CRC at the end.
  * `:output_usb` — USB, report `0x02`, 63 bytes, no CRC.

  Same 47-byte payload behind a different header, spliced from
  `Padwave.Schema.Common`. Sizes from the kernel:
  `DS_OUTPUT_REPORT_USB_SIZE` 63, `DS_OUTPUT_REPORT_BT_SIZE` 78.

  Only the fields whose `valid_flag*` bit is set are applied by the controller,
  which is why a lightbar-only update cannot disturb the audio configuration:
  leave the audio flags clear and the controller keeps them.
  """

  use Padwave.Wire

  packet :output, 0x31, size: 78, doc: "DualSense output report (Bluetooth)" do
    # 0x01  high nibble = counter 0..15, low nibble = 0
    u8(:seq_tag)
    # 0x02  must be 0x10
    u8(:tag)
    splice(Padwave.Schema.Common, :output_body)

    # 0x32..0x49
    bytes(:reserved4, 24)
    # 0x4a  CRC-32, seed 0xA2, little-endian
    u32(:crc)
  end

  packet :output_usb, 0x02, size: 63, doc: "DualSense output report (USB)" do
    splice(Padwave.Schema.Common, :output_body)

    # 0x30..0x3e
    bytes(:reserved4, 15)
  end
end
