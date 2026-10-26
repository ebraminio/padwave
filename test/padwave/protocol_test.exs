defmodule Padwave.ProtocolTest do
  @moduledoc """
  The audio and feature descriptions are documentation, but they are compiled
  and size-checked, so a wrong offset is a test failure rather than a stray
  comment.
  """

  use ExUnit.Case, async: true

  alias Padwave.Schema.{DualSenseAudio, DualSenseFeature}

  defp put(binary, index, value) do
    <<head::binary-size(index), _old, tail::binary>> = binary
    <<head::binary, value, tail::binary>>
  end

  test "the audio report covers all 398 bytes" do
    spec = DualSenseAudio.spec(:audio_out)
    assert spec.id == 0x36
    assert spec.size == 398
    assert 1 + Enum.reduce(spec.fields, 0, &(&1.size + &2)) == 398
  end

  test "a 0x36 report decodes into its four sub-packets" do
    packet =
      :binary.copy(<<0>>, 398)
      |> put(0, 0x36)
      |> put(1, 0xD0)
      |> put(2, 0x91)
      |> put(3, 7)
      |> put(4, 0xFE)
      |> put(11, 0x90)
      |> put(12, 63)
      |> put(13, 0xF1)
      |> put(14, 0xE3)
      |> put(76, 0x92)
      |> put(77, 64)
      |> put(142, 0x93)
      |> put(143, 200)

    fields = DualSenseAudio.decode_audio_out(packet)

    assert fields.audio_enable_mask == 0xFE
    assert fields.state_length == 63

    # the snapshot is a full output payload: its first two bytes are the
    # owned_fields mask, and the 0xF1 low byte is what stops the audio stream
    # from claiming the triggers
    assert fields.owned_fields == 0xE3F1
    assert fields.haptics_length == 64
    assert fields.speaker_length == 200
    assert fields.speaker_tag == 0x93
  end

  test "the feature reports have the kernel's ids and lengths" do
    assert %{id: 0x05, size: 41} = DualSenseFeature.spec(:calibration)
    assert %{id: 0x09, size: 20} = DualSenseFeature.spec(:pairing)
    assert %{id: 0x20, size: 64} = DualSenseFeature.spec(:firmware)
  end

  test "the firmware offsets match where the kernel reads them" do
    offsets = Map.new(DualSenseFeature.spec(:firmware).fields, &{&1.name, &1.offset})

    assert offsets.hw_version == 24
    assert offsets.fw_version == 28
    assert offsets.update_version == 44
  end

  test "the calibration body is seventeen int16s plus six unknown bytes" do
    spec = DualSenseFeature.spec(:calibration)
    int16s = Enum.count(spec.fields, &(&1.kind == :i16le))

    assert int16s == 17
    assert offsets(spec) |> Map.fetch!(:gyro_pitch_bias) == 1
    assert offsets(spec) |> Map.fetch!(:acc_z_minus) == 33
  end

  defp offsets(spec), do: Map.new(spec.fields, &{&1.name, &1.offset})
end
