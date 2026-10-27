defmodule Padwave.InputLineTest do
  use ExUnit.Case, async: true

  alias Padwave.Input.Frame

  @line "frame seq=1 lx=129 ly=124 rx=124 ry=127 l2=0 r2=0 dpad=none buttons=cross,triangle " <>
          "battery=15 charging=0 jack=1 mic=1 temp=21 gx=-5 gy=-4 gz=0 ax=-55 ay=7808 " <>
          "az=-1909 tfl=0 tfr=0 t1=8a86d00c t2=875e3019"

  test "a padwave input line becomes the frame the mapper expects" do
    frame = Frame.from_line(@line)

    assert frame.buttons == [:cross, :triangle]
    assert frame.dpad == :none
    assert frame.lx == 129
    assert frame.battery == 15
    assert frame.charging == :discharging
    assert frame.jack and frame.mic
    assert frame.temperature == 21
    assert frame.g_force > 0.9
    assert frame.fields.accel_y == 7808
    assert frame.fields.gyro_x == -5
    assert [%{active: false, id: 10, x: 134, y: 205}, %{active: false}] = frame.touches
  end

  test "an unknown line is ignored rather than crashing" do
    assert Frame.from_line("something else") == nil
  end
end
