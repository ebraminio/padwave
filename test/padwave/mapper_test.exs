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

  test "triangle toggles pause" do
    Simulator.press!(%{buttons: ["triangle"]}, 80)
    assert_receive {:padwave, {:action, :toggle_pause}}, 1000
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

  test "the Edge back paddles skip stations like L1/R1" do
    Simulator.press!(%{buttons: ["left_paddle"]}, 80)
    assert_receive {:padwave, {:action, :previous}}, 1000

    Simulator.press!(%{buttons: ["right_paddle"]}, 80)
    assert_receive {:padwave, {:action, :next_random}}, 1000
  end

  test "the Edge Fn buttons pause and mute" do
    Simulator.press!(%{buttons: ["fn1"]}, 80)
    assert_receive {:padwave, {:action, :toggle_pause}}, 1000

    Simulator.press!(%{buttons: ["fn2"]}, 80)
    assert_receive {:padwave, {:action, :toggle_mute}}, 1000
  end

  test "circle is the play button (the broken cross is unbound)" do
    Simulator.press!(%{buttons: ["circle"]}, 80)
    assert_receive {:padwave, {:action, :play}}, 1000
  end

  test "the right trigger nudges the volume up and the left one down" do
    Simulator.press!(%{r2: 240}, 400)
    assert_receive {:padwave, {:action, {:volume, step}}} when step > 0, 1_000

    Simulator.press!(%{l2: 240}, 400)
    assert_receive {:padwave, {:action, {:volume, step}}} when step < 0, 1_000
  end

  test "touchpad click toggles mute" do
    Simulator.press!(%{buttons: ["touchpad"]}, 80)
    assert_receive {:padwave, {:action, :toggle_mute}}, 1_000
  end

  test "square picks a station from another game" do
    Simulator.press!(%{buttons: ["square"]}, 80)
    assert_receive {:padwave, {:action, :shuffle_game}}, 1_000
  end
end
