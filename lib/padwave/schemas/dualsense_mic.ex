defmodule Padwave.Schema.DualSenseMic do
  @moduledoc """
  Microphone frames travelling in the `0x31` input stream.

  The DualSense has no audio class over Bluetooth: the microphone is multiplexed
  into the same HID reports as the gamepad, and its audio is Opus, not PCM.
  This module is the schema for those frames — what they look like, how to tell
  them apart, and what has to be true before the controller sends any.

  ## Turning it on

  Three separate things, all written in the outgoing `0x36` audio report (see
  `Padwave.Schema.DualSenseAudio`):

  | # | field | value | why |
  |---|---|---|---|
  | 1 | `power_save_control` (`+0x09`) | bit 4 = 0, **and** `valid_flag1` bit 1 claims the byte | unmutes the mic; without the claim the write is inert |
  | 2 | audio enable mask (`0x36` byte 4) | bit 0 set: `0xFF` instead of `0xFE` | asks the controller to start sending capture frames |
  | 3 | `mic_volume` (`+0x06`) | `0x40`, claimed by `valid_flag0` bit 6 | otherwise the frames are silence |

  Step 2 is the tap; step 1 is the one people forget, because the controller
  happily accepts an unmute for a field nobody claimed.

  ## What comes back

  Still report id `0x31`, but **not** the state report: byte 3 is the constant
  Opus table-of-contents byte `0xD4`. A state report has the sequence/tag
  nibbles there instead, and that is the whole demultiplexing trick:

      <<0x31, seq, tag, 0xD4, opus::binary, crc::little-32>>

  | property | value |
  |---|---|
  | codec | Opus |
  | sample rate | 48 000 Hz |
  | channels | mono |
  | frame | 480 samples = 10 ms |
  | packet rate | ~100 per second |
  | trailing bytes | CRC-32, seed `0xA1` |

  This module is the schema and the demultiplexer: `frame?/1` recognises the
  frames, `payload/1` returns the Opus packet with the CRC stripped, and
  `valid?/1` adds the CRC test. `Padwave.Mic` is what turns those packets into
  a level for the panel, through a small libopus port helper; it deliberately
  does not play them, because the speaker is the audio service's.

  ## Caveats

  * **It disturbs the gamepad input.** These frames carry the same report id as
    the state report, so anything that assumes every `0x31` is 78 bytes reads
    Opus bytes as sticks and buttons. Padwave drops them by length, but they
    still consume link time and can delay state frames.
  * **The kernel ignores them.** `hid-playstation` never parses the audio
    blocks, so there is no ALSA capture device; the `ds5_mic` PipeWire source in
    LinuxAudio4Dualsense5 is the only way to hear it.
  * **Both directions share the link.** Speaker down and mic up are each roughly
    160 kbps of Opus; duplex nearly doubles the Bluetooth traffic.
  """

  alias Padwave.Input.Crc

  @report 0x31
  @toc 0xD4
  @header_size 3
  @crc_size 4

  @doc "The HID report id these frames arrive in (the same as gamepad state)."
  def report_id, do: @report

  @doc "The marker byte that distinguishes a mic frame from a state report."
  def toc, do: @toc

  @doc "Decoded audio parameters, for anything that wants to decode."
  def audio, do: %{codec: :opus, channels: 1, sample_rate: 48_000, samples: 480, frame_ms: 10}

  @doc """
  True when a report is a microphone frame rather than gamepad state.

  The check is the `0xD4` TOC at byte 3; the length is part of it only so a
  three-byte packet cannot match.
  """
  def frame?(<<@report, _seq, _tag, @toc, _::binary>>), do: true
  def frame?(_report), do: false

  @doc """
  The Opus packet inside a mic frame: everything after the TOC byte, minus the
  trailing CRC. Returns `:error` for anything that is not a frame.
  """
  def payload(report) when is_binary(report) do
    size = byte_size(report)

    if frame?(report) and size > @header_size + @crc_size do
      binary_part(report, @header_size, size - @header_size - @crc_size)
    else
      :error
    end
  end

  def payload(_report), do: :error

  @doc """
  True when the frame is a mic frame *and* its trailing CRC-32 (seed `0xA1`,
  same as any Bluetooth input report) matches.
  """
  def valid?(report), do: frame?(report) and Crc.valid_input?(report)

  @doc "Metadata for `/api/schema`, in the same shape as the packet specs."
  def spec do
    %{
      name: :mic,
      id: @report,
      size: :variable,
      doc: "Microphone Opus frames carried by the 0x31 input report",
      toc: @toc,
      crc: %{size: @crc_size, seed: 0xA1},
      audio: audio()
    }
  end
end
