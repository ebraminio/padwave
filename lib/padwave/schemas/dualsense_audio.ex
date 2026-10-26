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

  ## Enabling the microphone

  There is no audio-class interface over Bluetooth, so the microphone shares
  this same HID duplex. To capture from it:

  1. Set bit 0 of the audio enable mask at `0x004`: `0xFF` instead of `0xFE`.
  2. Don't mute it in the state snapshot — `power_save_control` (`+0x09`) must
     have its 0x10 bit clear, and `mic_volume` (`+0x06`) must be non-zero
     (0x40 is full) with the `:mic_volume` bit of `owned_fields` set so the
     value is adopted.
  3. Demux the input. While duplex is on, the controller interleaves frames
     that keep report id `0x31` but carry the Opus TOC `0xD4` at report byte 3
     (input payload `+0x01`). Those are Opus 48 kHz mono, one 10 ms frame each:
     skip them when reading buttons, decode them to get sound.
  4. Expect a busy link. Duplex roughly doubles the traffic, and anything that
     parses every `0x31` as gamepad data will see phantom sticks — the stick
     fields in those frames are Opus bytes, not positions.

  ## The state snapshot is a whole output payload

  Sub-packet `0x10` is not opaque bytes: its first 47 bytes are byte-for-byte
  the `0x31` output payload — `owned_fields`, volumes, trigger effect blocks,
  lightbar — followed by sixteen bytes of padding. So the audio
  stream re-states the controller's whole output configuration 93 times a
  second, and every field it does not own must have its valid bit **clear**, or
  this copy silently overwrites whatever another writer set.

  That is exactly what went wrong here: the snapshot's low byte was `0xFD` —
  bits 2 and 3 set, i.e. *both trigger effects claimed* — with a zeroed trigger
  block, so every audio frame cancelled the vibration padwave had just applied.
  It now sends `0xF1`, which claims volumes and audio routing but leaves the
  triggers alone. The lightbar was never affected because the high byte
  (`0xE3`) does not claim the LED colour bit.
  """

  use Padwave.Wire

  packet :audio_out, 0x36, size: 398, doc: "DualSense audio report (Bluetooth)" do
    u8 :seq_tag                  # 0x001  high nibble = counter, low nibble = 0

    # -- sub-packet 0x11: audio configuration -----------------------------
    u8 :config_tag               # 0x002  0x11 | 0x80 (more sub-packets follow)
    u8 :config_length            # 0x003  7
    u8 :audio_enable_mask        # 0x004  bit 0 = microphone capture: 0xFE keeps
                                 #        the mic shut, 0xFF opens it (the input
                                 #        stream then also carries 0xD4 frames)
    bytes :haptic_lengths, 5     # 0x005..0x009  64 each
    u8 :counter                  # 0x00a  free-running 8-bit counter

    # -- sub-packet 0x10: state snapshot ----------------------------------
    u8 :state_tag                # 0x00b  0x10 | 0x80
    u8 :state_length             # 0x00c  63

    # The snapshot is the 0x31 output payload, spliced so its fields are named
    # here too. Note owned_fields: claiming :right_trigger or :left_trigger
    # here hands the trigger effect to this stream, which writes zeros to it
    # 93 times a second. The fix for the cancelled trigger effects was clearing
    # exactly those two bits (0xFD -> 0xF1).
    splice Padwave.Schema.DualSenseOutput.Body, :output_body
    bytes :state_padding, 16     # 0x03c..0x04b  the rest of the 63-byte block

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
