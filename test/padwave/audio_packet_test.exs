defmodule Padwave.AudioPacketTest do
  use ExUnit.Case, async: true

  alias Padwave.Output.Audio
  alias Padwave.Schema.DualSenseAudio

  # 200 bytes of "Opus" — the builder does not care about the contents, and the
  # point of the test is the envelope around them.
  defp opus, do: :binary.copy(<<0xD4>>, 200)

  test "builds a 398-byte 0x36 with a CRC seeded with 0xA2" do
    packet = Audio.build(opus(), output: :jack)

    assert byte_size(packet) == 398
    assert <<0x36, _seq::4, 0::4, rest::binary>> = packet
    assert byte_size(rest) == 396

    <<body::binary-size(394), crc::little-32>> = packet
    assert :erlang.crc32(<<0xA2>> <> body) == crc
    assert Audio.valid?(packet)
  end

  test "the jack and speaker variants differ only in routing" do
    jack = Audio.build(opus(), output: :jack)
    speaker = Audio.build(opus(), output: :speaker)

    jack_fields = DualSenseAudio.decode_audio_out(jack)
    speaker_fields = DualSenseAudio.decode_audio_out(speaker)

    assert jack_fields.speaker_tag == 0x96
    assert speaker_fields.speaker_tag == 0x93
    assert jack_fields.audio_control == 0x00
    assert speaker_fields.audio_control == 0x09
    refute jack_fields.valid_flag0.right_trigger
    assert jack_fields.valid_flag0.headphone_volume
    refute jack_fields.audio_enable_mask.microphone_capture
    assert jack_fields.speaker_length == 200
  end

  test "a corrupted packet fails the validity check" do
    packet = Audio.build(opus())
    <<head::binary-size(200), byte, tail::binary>> = packet
    refute Audio.valid?(head <> <<Bitwise.bxor(byte, 0xFF)>> <> tail)
    refute Audio.valid?(binary_part(packet, 0, 100))
  end
end
