defmodule Padwave.MapperTest do
  @moduledoc """
  End-to-end input tests: synthetic 0x31 reports go in through the same
  `hidraw` decode path the controller uses, and we assert the actions that
  come out of the other side.
  """

  use ExUnit.Case, async: false

  alias Padwave.Bus
  alias Padwave.Input.Simulator

  setup do
    Bus.subscribe()
    :ok
  end

  test "pulling the headset out asks for a pause, and plugging it back asks for play" do
    Simulator.press!(%{jack: true}, 400)
    Simulator.press!(%{jack: false}, 400)
    assert_receive {:padwave, {:action, :jack_unplugged}}, 1500

    Simulator.press!(%{jack: true}, 400)
    assert_receive {:padwave, {:action, :jack_plugged}}, 1500
  end

  test "a one-frame jack flicker is ignored" do
    Simulator.press!(%{jack: true}, 400)
    # one frame out, the very next frame back in: under the debounce window
    Simulator.frame_once!(%{jack: false})
    Simulator.frame_once!(%{jack: true})

    refute_receive {:padwave, {:action, :jack_unplugged}}, 600
  end

end
