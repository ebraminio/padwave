defmodule Padwave.Schema.DualSenseOutput do
  @moduledoc """
  DualSense output report `0x31`, Bluetooth flavour (78 bytes).

  This is the packet that drives the lightbar, the player LEDs, the haptics and
  the audio routing. LinuxAudio4Dualsense5 uses report `0x36` for streaming
  audio (a variant with sub-packets); `0x31` is the plain state report.

  Layout taken from `drivers/hid/hid-playstation.c`
  (`struct dualsense_output_report_common`) and verified against a packet
  captured from the kernel itself with `btmon` — see
  `test/padwave/output_report_test.exs`, which reproduces that exact packet,
  CRC included, byte for byte.

  Only the fields whose `valid_flag*` bit is set are applied by the
  controller, which is why a lightbar-only update cannot disturb the audio
  configuration: leave the audio flags clear and the controller keeps them.
  """

  use Padwave.Wire

  packet :output, 0x31, size: 78, doc: "DualSense output report (Bluetooth)" do
    # -- header (Bluetooth only) -----------------------------------------
    # 0x01  high nibble = counter 0..15, low nibble = 0
    u8(:seq_tag)
    # 0x02  must be 0x10
    u8(:tag)

    # -- validity flags --------------------------------------------------
    # A field is only applied when its flag is set, which is what makes a
    # lightbar-only update safe while LinuxAudio4Dualsense5 streams audio.
    # 0x03  bit0 rumble, bit1 haptics, bit5 speaker vol,
    u8(:valid_flag0)
    #       bit6 mic vol, bit7 audio routing
    # 0x04  bit0 mic LED, bit1 power save, bit2 lightbar,
    u8(:valid_flag1)
    #       bit3 release LEDs, bit4 player LEDs,
    #       bit7 audio control 2
    # 0x05  DS4-compatible rumble
    u8(:motor_right)
    # 0x06
    u8(:motor_left)
    # 0x07  0x00..0x7f
    u8(:headphone_volume)
    # 0x08  0x00..0xff
    u8(:speaker_volume)
    # 0x09  0x00..0x40
    u8(:mic_volume)
    # 0x0a  output path select (speaker / headset)
    u8(:audio_control)
    # 0x0b
    u8(:mute_button_led)
    # 0x0c
    u8(:power_save_control)
    # 0x0d..0x27
    bytes(:reserved2, 27)
    # 0x28  speaker preamp gain
    u8(:audio_control2)
    # 0x29  bit1 lightbar setup, bit2 vibration v2
    u8(:valid_flag2)
    # 0x2a..0x2b
    bytes(:reserved3, 2)

    # -- LEDs ------------------------------------------------------------
    # 0x2c  fade-out after power-on
    u8(:lightbar_setup)
    # 0x2d
    u8(:led_brightness)
    # 0x2e  bitmask of the five white LEDs
    u8(:player_leds)
    # 0x2f
    u8(:lightbar_red)
    # 0x30
    u8(:lightbar_green)
    # 0x31
    u8(:lightbar_blue)

    # 0x32..0x49
    bytes(:reserved4, 24)
    # 0x4a  CRC-32, seed 0xA2, little-endian
    u32(:crc)
  end
end
