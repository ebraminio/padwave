defmodule Padwave.Schema.DualSenseOutput.Body do
  @moduledoc """
  The 47-byte output payload shared by the USB and Bluetooth reports.

  `+0x00` in the comments is the offset inside this payload: 0x01 in a USB
  report, 0x03 in a Bluetooth one (after the id, the sequence tag and the 0x10
  tag byte).
  """

  use Padwave.Wire

  field_list :output_body do
    # Which fields this report owns, as one 16-bit little-endian mask (bits 0..7
    # are the byte at +0x00, 8..15 the byte at +0x01). A set bit means "adopt
    # my bytes for this feature"; a clear bit means "leave that feature as it
    # is". Setting a bit is a claim, not a change notification: the bytes are
    # taken as written, zeros included.
    bits :owned_fields,
      rumble: 1,                   # +0x00 bit 0
      haptics: 1,                  #       bit 1
      right_trigger: 1,            #       bit 2
      left_trigger: 1,             #       bit 3
      headphone_volume: 1,         #       bit 4
      speaker_volume: 1,           #       bit 5
      mic_volume: 1,               #       bit 6
      audio_routing: 1,            #       bit 7
      mute_led: 1,                 # +0x01 bit 0
      audio_mute: 1,               #       bit 1
      lightbar: 1,                 #       bit 2
      reset_lights: 1,             #       bit 3
      player_leds: 1,              #       bit 4
      haptic_lpf: 1,               #       bit 5
      motor_power_level: 1,        #       bit 6
      audio_control2: 1            #       bit 7

    u8 :motor_right              # +0x02  DS4-compatible rumble
    u8 :motor_left               # +0x03
    u8 :headphone_volume         # +0x04  0x00..0x7f
    u8 :speaker_volume           # +0x05  0x00..0xff
    u8 :mic_volume               # +0x06  0x00..0x40 (claimed by :mic_volume)
    u8 :audio_control            # +0x07  output path select (speaker / headset;
                                 #        claimed by :audio_routing)

    u8 :mute_button_led          # +0x08
    bits :power_save_control,
      reserved_0: 4,               # +0x09 bits 0-3
      microphone_mute: 1,          #       bit 4
      reserved_1: 1,               #       bit 5
      audio_mute: 1,               #       bit 6
      reserved_2: 1                #       bit 7

    # Adaptive trigger effects: a mode byte and ten parameter bytes per
    # trigger, enabled by the :right_trigger / :left_trigger bits. Described
    # here, not driven
    # yet.
    u8 :right_trigger_mode       # +0x0a  0x05 off, 0x21 feedback, 0x25 weapon,
                                 #        0x26 vibration (0x22/0x23/0x27 unofficial)
    bytes :right_trigger_params, 10   # +0x0b..0x14  zones + 3-bit strength,
                                      #        vibration frequency at byte 9
    u8 :left_trigger_mode        # +0x15
    bytes :left_trigger_params, 10    # +0x16..0x1f

    bytes :reserved_32, 4        # +0x20..0x23
    u8 :haptic_volume            # +0x24
    u8 :audio_control2           # +0x25  speaker preamp gain
    bits :valid_flag2,
      brightness: 1,               # +0x26 bit 0
      color_fade: 1,               #       bit 1
      improved_rumble: 1,          #       bit 2
      reserved_0: 5                #       bits 3-7
    bytes :reserved_39, 2        # +0x27..0x28

    u8 :lightbar_setup           # +0x29  fade-out after power-on
    u8 :led_brightness           # +0x2a
    bits :player_leds,
      led1: 1,                     # +0x2b bit 0
      led2: 1,                     #       bit 1
      led3: 1,                     #       bit 2
      led4: 1,                     #       bit 3
      led5: 1,                     #       bit 4
      reserved_0: 3                #       bits 5-7
    u8 :lightbar_red             # +0x2c
    u8 :lightbar_green           # +0x2d
    u8 :lightbar_blue            # +0x2e
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
    u8 :seq_tag                  # 0x01  high nibble = counter 0..15, low nibble = 0
    u8 :tag                      # 0x02  must be 0x10
    splice Padwave.Schema.DualSenseOutput.Body, :output_body

    bytes :reserved4, 24         # 0x32..0x49
    u32le :crc                   # 0x4a  CRC-32, seed 0xA2, little-endian
  end

  packet :output_usb, 0x02, size: 63, doc: "DualSense output report (USB)" do
    splice Padwave.Schema.DualSenseOutput.Body, :output_body

    bytes :reserved4, 15         # 0x30..0x3e
  end
end
