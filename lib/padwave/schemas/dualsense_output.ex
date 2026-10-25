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
    u8(:seq_tag)
    u8(:tag)
    u8(:valid_flag0)
    u8(:valid_flag1)
    u8(:motor_right)
    u8(:motor_left)
    u8(:headphone_volume)
    u8(:speaker_volume)
    u8(:mic_volume)
    u8(:audio_control)
    u8(:mute_button_led)
    u8(:power_save_control)
    bytes(:reserved2, 27)
    u8(:audio_control2)
    u8(:valid_flag2)
    bytes(:reserved3, 2)
    u8(:lightbar_setup)
    u8(:led_brightness)
    u8(:player_leds)
    u8(:lightbar_red)
    u8(:lightbar_green)
    u8(:lightbar_blue)
    bytes(:reserved4, 24)
    u32(:crc)
  end
end
