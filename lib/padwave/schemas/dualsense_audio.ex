defmodule Padwave.Schema.DualSenseAudio do
  @moduledoc """
  DualSense audio report `0x36` — the undocumented Bluetooth speaker path.

  Sony's position is that the speaker only works over USB; over Bluetooth the
  controller speaks a proprietary Opus-in-HID protocol. This is the report that
  LinuxAudio4Dualsense5 reverse-engineered and streams at 93.75 Hz
  (one packet every 512/48000 s), and it is described here so the whole picture
  lives in one place — padwave itself does not send it.

  Layout, verified against `reverse_engineered/ps5bt_membrane.py` (which builds
  it byte for byte) and the top-level README's protocol table:

    0x000  report id 0x36
    0x001  sequence tag, high nibble (a different counter from report 0x31)
    0x002  sub-packet 0x11 | 0x80 — audio configuration, length 7
    0x00b  sub-packet 0x10 | 0x80 — state snapshot, length 63
    0x04c  sub-packet 0x12 | 0x80 — haptics PCM, length 64
    0x08e  sub-packet 0x13 | 0x80 — speaker Opus (0x16 = headphone jack), 200
    0x158  50 bytes of padding
    0x18a  CRC-32, seed 0xA2, little-endian (same scheme as report 0x31)

  Three details are the whole difficulty of this protocol: the 10.667 ms frame
  interval (not 10 ms), the Opus CBR 160 kbps parameters, and the audio-enable
  mask's bit 0, which turns on microphone capture and floods the link if left
  set.

  The microphone comes back *inside* the normal `0x31` input report (marker
  `0xD4`, Opus 48 kHz mono, 10 ms) when duplex is enabled. That is why the
  audio service mutes the mic in its state snapshot: the input report is also
  where padwave reads buttons from, so a mic-enabled stream would double the
  link's traffic.
  """

  use Padwave.Wire

  packet :audio_out, 0x36, size: 398, doc: "DualSense audio report (Bluetooth)" do
    u8 :seq_tag                  # 0x001  high nibble = counter, low nibble = 0

    # -- sub-packet 0x11: audio configuration -----------------------------
    u8 :config_tag               # 0x002  0x11 | 0x80 (more sub-packets follow)
    u8 :config_length            # 0x003  7
    u8 :audio_enable_mask        # 0x004  0xFE — bit 0 clear keeps mic capture off
    bytes :haptic_lengths, 5     # 0x005..0x009  64 each
    u8 :counter                  # 0x00a  free-running 8-bit counter

    # -- sub-packet 0x10: state snapshot ----------------------------------
    u8 :state_tag                # 0x00b  0x10 | 0x80
    u8 :state_length             # 0x00c  63
    bytes :state_snapshot, 63    # 0x00d..0x04b  first 47 bytes mirror the 0x31
                                 #        output payload (flags, volumes, LEDs)

    # -- sub-packet 0x12: haptics (the "subwoofer") ------------------------
    u8 :haptics_tag              # 0x04c  0x12 | 0x80
    u8 :haptics_length           # 0x04d  64
    bytes :haptics_pcm, 64       # 0x04e..0x08d  64 × int8 PCM at 6 kHz

    # -- sub-packet 0x13: speaker, or 0x16 for the headphone jack ----------
    u8 :speaker_tag              # 0x08e  0x13 = membrane, 0x16 = jack
    u8 :speaker_length           # 0x08f  200
    bytes :opus_frame, 200       # 0x090..0x157  Opus 48 kHz stereo CBR 160 kbps

    bytes :padding, 50           # 0x158..0x189
    u32le :crc                   # 0x18a..0x18d  seed 0xA2
  end
end
