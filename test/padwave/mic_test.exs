defmodule Padwave.MicTest do
  use ExUnit.Case, async: true

  alias Padwave.Schema.DualSenseMic

  defp signed(frame) do
    crc = :erlang.crc32(<<0xA1>> <> frame)
    frame <> <<crc::little-32>>
  end

  test "mic frames are recognised by the 0xD4 marker" do
    assert DualSenseMic.frame?(signed(<<0x31, 0x10, 0x10, 0xD4, 1, 2, 3>>))
    refute DualSenseMic.frame?(signed(<<0x31, 0x10, 0x10, 0x00, 1, 2, 3>>))
    refute DualSenseMic.frame?(signed(<<0x01, 0x10, 0x10, 0xD4, 1, 2, 3>>))
    refute DualSenseMic.frame?(<<0x31, 0x10, 0x10>>)
  end

  test "the payload is the Opus packet without the header or the CRC" do
    report = signed(<<0x31, 0x10, 0x10, 0xD4, 0xAA, 0xBB, 0xCC>>)
    assert DualSenseMic.payload(report) == <<0xD4, 0xAA, 0xBB, 0xCC>>
    assert DualSenseMic.payload(<<0x31, 0x10, 0x10, 0x00, 1, 2, 3>>) == :error
    assert DualSenseMic.payload(<<0x31>>) == :error
  end

  test "the spec matches the other packet specs" do
    spec = DualSenseMic.spec()
    assert spec.id == 0x31
    assert spec.size == :variable
    assert spec.toc == 0xD4
    assert spec.audio == %{codec: :opus, channels: 1, sample_rate: 48_000, samples: 480, frame_ms: 10}
  end
end
