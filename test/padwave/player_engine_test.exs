defmodule Padwave.PlayerEngineTest do
  # The engine choice is application config, so this cannot run concurrently
  # with anything else that reads it.
  use ExUnit.Case, async: false

  alias Padwave.Player.Engine

  test "the seam picks the configured backend" do
    put(:mpv)
    assert Engine.backend() == Padwave.Player

    put(:ffmpeg)
    assert Engine.backend() == Padwave.Player.Ffmpeg

    put(:native)
    assert Engine.backend() == Padwave.Player.Native
  after
    put(:ffmpeg)
  end

  test "an unknown value falls back to the default engine rather than crashing" do
    put(:nonsense)
    assert Engine.backend() == Padwave.Player.Ffmpeg
  after
    put(:ffmpeg)
  end

  defp put(value), do: Application.put_env(:padwave, :player_engine, value)
end
