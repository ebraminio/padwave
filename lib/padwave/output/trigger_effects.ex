defmodule Padwave.Output.TriggerEffects do
  @moduledoc """
  Adaptive-trigger effect blocks, encoded the way the firmware actually expects.

  The block is eleven bytes: a mode, then ten mode-dependent parameter bytes.
  The official modes use ten trigger *zones* (position 0–9) with a 3-bit
  strength per zone; the values here follow the verified layouts documented by
  Nielk1 and mirrored in `DualSenseClient/Triggers/TriggerEffectBuilder.cs`.

  Two things are easy to get wrong, and both were:

    * **`0x05` is "off"**, not `0x00`. Sending `0x00` leaves the trigger's
      previous effect in place.
    * **Vibration is `0x26`, not `0x06`.** `0x06` is a leftover "simple
      vibration" mode from early firmware probing, with no parameter
      protection; it is not the official effect.

  Official modes:

  | mode | effect | parameters |
  |------|--------|------------|
  | `0x05` | off | — |
  | `0x21` | feedback (resistance from a position) | zones + 3-bit strength |
  | `0x25` | weapon (resistance between two positions, then release) | zones + strength |
  | `0x26` | vibration (buzzes while pushing past a position) | zones + 3-bit amplitude, frequency at byte 9 |
  | `0x22`/`0x23`/`0x27` | bow / galloping / machine — unofficial, may vanish | mode-specific |

  Zone packing: `active` is a 10-bit mask of zones the effect covers, and the
  strength of each zone is 3 bits, both little-endian at bytes 1–2 and 3–6.
  """

  import Bitwise

  @off 0x05
  @feedback 0x21
  @weapon 0x25
  @vibration 0x26

  @doc "Turns the effect off and returns the trigger stop to neutral."
  def off, do: {@off, <<0::size(80)>>}

  @doc """
  Vibration: the trigger buzzes while pushed past `position`.

  `position` 0–9, `amplitude` 1–8 (0 turns it off), `frequency` in Hz 1–255.
  """
  def vibration(position \\ 0, amplitude \\ 8, frequency \\ 25) do
    position = clamp(position, 0, 9)
    amplitude = clamp(amplitude, 0, 8)

    cond do
      amplitude == 0 or frequency == 0 ->
        off()

      true ->
        {active, zones} = pack(position, amplitude)
        {@vibration, <<active::little-16, zones::little-32, 0, 0, clamp(frequency, 0, 255), 0>>}
    end
  end

  @doc "Resistance from `position` onwards. `strength` 1–8, `position` 0–9."
  def feedback(position \\ 2, strength \\ 8) do
    position = clamp(position, 0, 9)
    strength = clamp(strength, 0, 8)

    if strength == 0 do
      off()
    else
      {active, zones} = pack(position, strength)
      {@feedback, <<active::little-16, zones::little-32, 0, 0, 0, 0>>}
    end
  end

  @doc """
  Weapon: resistance from `start` up to `stop`, then the trigger pulls free.
  `start` 2–7, `stop` start+1..8, `strength` 1–8.
  """
  def weapon(start_position \\ 2, stop_position \\ 6, strength \\ 8) do
    start_position = clamp(start_position, 0, 9)
    stop_position = clamp(stop_position, 0, 9)
    strength = clamp(strength, 0, 8)

    if strength == 0 do
      off()
    else
      zones = 1 <<< start_position ||| 1 <<< stop_position
      {@weapon, <<zones::little-16, strength - 1, 0, 0, 0, 0, 0, 0>>}
    end
  end

  @doc "Human name for a mode byte."
  def name(@off), do: :off
  def name(0x21), do: :feedback
  def name(0x25), do: :weapon
  def name(0x26), do: :vibration
  def name(0x22), do: :bow
  def name(0x23), do: :galloping
  def name(0x27), do: :machine
  def name(_), do: :unknown

  defp pack(position, strength) do
    Enum.reduce(position..9, {0, 0}, fn zone, {active, zones} ->
      {active ||| 1 <<< zone, zones ||| (strength - 1 &&& 0x07) <<< (3 * zone)}
    end)
  end

  defp clamp(value, min, max) when is_integer(value), do: value |> max(min) |> min(max)
  defp clamp(_value, min, _max), do: min
end
