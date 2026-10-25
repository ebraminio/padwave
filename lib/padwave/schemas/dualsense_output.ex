defmodule Padwave.Schema.DualSenseOutput.Body do
  @moduledoc """
  The 47-byte output payload shared by the USB and Bluetooth reports.

  `+0x00` in the comments is the offset inside this payload: 0x01 in a USB
  report, 0x03 in a Bluetooth one (after the id, the sequence tag and the 0x10
  tag byte).
  """

  use Padwave.Wire

  field_list :output_body do
    # A field is only applied when its flag bit is set.
    # +0x00  bit0 rumble, bit1 haptics, bit2 right trigger,
    u8(:valid_flag0)
    #        bit3 left trigger, bit5 speaker volume,
    #        bit6 mic volume, bit7 audio routing
    # +0x01  bit0 mic LED, bit1 power save, bit2 lightbar,
    u8(:valid_flag1)
    #        bit3 release LEDs, bit4 player LEDs, bit7 audio 2
    # +0x02  DS4-compatible rumble
    u8(:motor_right)
    # +0x03
    u8(:motor_left)
    # +0x04  0x00..0x7f
    u8(:headphone_volume)
    # +0x05  0x00..0xff
    u8(:speaker_volume)
    # +0x06  0x00..0x40
    u8(:mic_volume)
    # +0x07  output path select (speaker / headset)
    u8(:audio_control)

    # +0x08
    u8(:mute_button_led)
    # +0x09
    u8(:power_save_control)

    # Adaptive trigger effects: a mode byte and ten parameter bytes per
    # trigger, enabled by valid_flag0 bits 2 and 3. Described here, not driven
    # yet — see the README.
    # +0x0a  0x00 off, 0x01 resistance, 0x02 pulse, ...
    u8(:right_trigger_mode)
    # +0x0b..0x14
    bytes(:right_trigger_params, 10)
    # +0x15
    u8(:left_trigger_mode)
    # +0x16..0x1f
    bytes(:left_trigger_params, 10)

    # +0x20..0x23
    bytes(:reserved_32, 4)
    # +0x24
    u8(:haptic_volume)
    # +0x25  speaker preamp gain
    u8(:audio_control2)
    # +0x26  bit1 lightbar setup, bit2 vibration v2
    u8(:valid_flag2)
    # +0x27..0x28
    bytes(:reserved_39, 2)

    # +0x29  fade-out after power-on
    u8(:lightbar_setup)
    # +0x2a
    u8(:led_brightness)
    # +0x2b  bitmask of the five white LEDs
    u8(:player_leds)
    # +0x2c
    u8(:lightbar_red)
    # +0x2d
    u8(:lightbar_green)
    # +0x2e
    u8(:lightbar_blue)
  end
end

defmodule Padwave.Schema.DualSenseOutput do
  @moduledoc """
  DualSense output reports, USB and Bluetooth.

  * `:output` — Bluetooth, report `0x31`, 78 bytes, CRC at the end.
  * `:output_usb` — USB, report `0x02`, 63 bytes, no CRC.

  Same 47-byte payload, described once in
  `Padwave.Schema.DualSenseOutput.Body` and spliced into both packets. Sizes
  from the kernel: `DS_OUTPUT_REPORT_USB_SIZE` 63, `DS_OUTPUT_REPORT_BT_SIZE`
  78.

  Only the fields whose `valid_flag*` bit is set are applied by the controller,
  which is what makes a lightbar-only update safe while LinuxAudio4Dualsense5
  streams audio: leave the audio flags clear and the controller keeps them.
  """

  use Padwave.Wire

  packet :output, 0x31, size: 78, doc: "DualSense output report (Bluetooth)" do
    # 0x01  high nibble = counter 0..15, low nibble = 0
    u8(:seq_tag)
    # 0x02  must be 0x10
    u8(:tag)
    splice(Padwave.Schema.DualSenseOutput.Body, :output_body)

    # 0x32..0x49
    bytes(:reserved4, 24)
    # 0x4a  CRC-32, seed 0xA2, little-endian
    u32(:crc)
  end

  packet :output_usb, 0x02, size: 63, doc: "DualSense output report (USB)" do
    splice(Padwave.Schema.DualSenseOutput.Body, :output_body)

    # 0x30..0x3e
    bytes(:reserved4, 15)
  end
end
