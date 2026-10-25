defmodule Padwave.TriggerEffectsTest do
  use ExUnit.Case, async: true

  import Bitwise

  alias Padwave.Output.TriggerEffects

  test "off is 0x05 with a zero parameter block" do
    assert {0x05, <<0::size(80)>>} = TriggerEffects.off()
  end

  test "vibration uses the official 0x26 mode and zone packing" do
    {mode, params} = TriggerEffects.vibration(2, 6, 30)

    assert mode == 0x26
    assert byte_size(params) == 10

    <<active::little-16, zones::little-32, 0, 0, frequency, 0>> = params

    # zones 2..9 are active, 0..1 are not
    assert active == 0b1111111100
    assert frequency == 30

    # each active zone carries amplitude - 1 = 5, three bits each
    for zone <- 2..9 do
      assert (bsr(zones, 3 * zone) &&& 0x07) == 5
    end

    assert (bsr(zones, 0) &&& 0x07) == 0
  end

  test "an amplitude of zero turns the effect off instead of buzzing at nothing" do
    assert {0x05, _} = TriggerEffects.vibration(0, 0, 30)
  end

  test "feedback is 0x21 and covers zones from the chosen position" do
    {mode, params} = TriggerEffects.feedback(4, 8)
    <<active::little-16, zones::little-32, _rest::binary>> = params

    assert mode == 0x21
    assert active == 0b1111110000
    assert (bsr(zones, 3 * 4) &&& 0x07) == 7
    assert (bsr(zones, 3 * 3) &&& 0x07) == 0
  end

  test "weapon marks the start and stop zones" do
    {mode, params} = TriggerEffects.weapon(2, 6, 8)
    <<zones::little-16, strength, _rest::binary>> = params

    assert mode == 0x25
    assert zones == (1 <<< 2 ||| 1 <<< 6)
    assert strength == 7
  end
end
