defmodule Padwave.ConfigTest do
  use ExUnit.Case, async: false

  alias Padwave.Config

  setup do
    before = Config.get(:idle_timeout_ms)
    on_exit(fn -> Config.update(%{"idle_timeout_ms" => to_string(before)}) end)
    :ok
  end

  test "accepts numeric strings, the way a form or query parameter arrives" do
    assert {:ok, config} = Config.update(%{"idle_timeout_ms" => "123000"})
    assert config["idle_timeout_ms"] == 123_000
    assert Config.get(:idle_timeout_ms) == 123_000
  end

  test "rejects unknown or read-only keys instead of ignoring them" do
    assert {:error, message} = Config.update(%{"definitely_not_a_setting" => 1})
    assert message =~ "unknown"
  end

  test "rejects a value it cannot coerce" do
    assert {:error, _message} = Config.update(%{"idle_timeout_ms" => "soon"})
  end

  test "the published config only exposes whitelisted keys" do
    config = Config.public()
    assert Map.has_key?(config, "idle_timeout_ms")
    refute Map.has_key?(config, "music_roots")
  end
end
