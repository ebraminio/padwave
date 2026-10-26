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

  test "d-pad up asks for the next station" do
    Simulator.press!(%{dpad: "up"}, 80)
    assert_receive {:padwave, {:action, :next_random}}, 1000
  end

  test "d-pad down goes back" do
    Simulator.press!(%{dpad: "down"}, 80)
    assert_receive {:padwave, {:action, :previous}}, 1000
  end

  test "a tap on d-pad right seeks forward" do
    Simulator.press!(%{dpad: "right"}, 60)
    assert_receive {:padwave, {:action, {:seek, 6}}}, 1000
  end

  test "holding d-pad right keeps steering, with growing steps" do
    Simulator.press!(%{dpad: "right"}, 1_100)

    assert_receive {:padwave, {:action, {:seek, 6}}}, 800
    assert_receive {:padwave, {:action, {:seek, 10}}}, 1_200
    assert_receive {:padwave, {:action, {:seek, 20}}}, 1_200
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
