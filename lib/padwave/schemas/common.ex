defmodule Padwave.Schema.Common do
  @moduledoc """
  The payload both transports share.

  A DualSense speaks the same 63-byte input payload and the same 47-byte output
  payload over USB and Bluetooth; only the framing differs — report id, the
  Bluetooth sequence tag, a Bluetooth-only tail, and (on output) a CRC.

  Declaring the payload once and splicing it into each packet means the two
  transports cannot drift apart: an offset fix lands in both, and the tests
  assert that USB offsets are exactly the Bluetooth ones minus the header.

  This is the analogue of embedding a common struct in harfbuzz — but the
  splice happens at compile time into a single binary match, so there is no
  offset arithmetic at runtime and no pointer chasing.
  """

  use Padwave.Wire

  # 63 bytes. Sits at offset 1 in a USB report (after the id) and at offset 2 in
  # a Bluetooth report (after the id and the sequence tag).
  field_list :input_body do
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

    # +0x28  L2 trigger status as the pad reports it
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

  # 47 bytes. Sits at offset 1 in a USB report and at offset 3 in a Bluetooth
  # report (after the id, the sequence tag and the 0x10 tag byte).
  field_list :output_body do
    # A field is only applied when its flag bit is set, which is what makes a
    # lightbar-only update safe while the audio sink streams.
    # +0x00  bit0 rumble, bit1 haptics, bit5 speaker vol,
    u8(:valid_flag0)
    #        bit6 mic vol, bit7 audio routing
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
    # +0x07  output path select
    u8(:audio_control)
    # +0x08
    u8(:mute_button_led)
    # +0x09
    u8(:power_save_control)
    # +0x0a..0x24
    bytes(:reserved2, 27)
    # +0x25  speaker preamp gain
    u8(:audio_control2)
    # +0x26  bit1 lightbar setup, bit2 vibration v2
    u8(:valid_flag2)
    # +0x27..0x28
    bytes(:reserved3, 2)

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
