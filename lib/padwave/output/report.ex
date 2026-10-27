defmodule Padwave.Output.Report do
  @moduledoc """
  Field maps for the output writer.

  The bytes are built in C++ (`scripts/padwave_schemas.hh`), so this module only
  says *what* to change: the names and values the `fields` command carries. The
  names are the `OutputBody` members and the `valid_flag*` enumerators from that
  header, which is the one description of the wire format.
  """

  @doc """
  A lightbar update.

  Options: `:brightness` also claims `valid_flag2` bit 0 and sends
  `led_brightness`, which a colour-only report does not.
  """
  def lightbar_fields(red, green, blue, opts \\ []) do
    fields = %{
      valid_flag1: [:lightbar],
      lightbar_red: red,
      lightbar_green: green,
      lightbar_blue: blue
    }

    case Keyword.get(opts, :brightness) do
      nil -> fields
      value -> Map.merge(fields, %{valid_flag2: [:brightness], led_brightness: value})
    end
  end

  @doc "A player-LED update. `mask` uses bits 0..4 for the five LEDs."
  def player_leds_fields(mask), do: %{valid_flag1: [:player_leds], player_leds: mask}

  @doc """
  A trigger effect: `mode` plus up to ten parameters.

  The official mode bytes are `0x05` off, `0x21` feedback, `0x25` weapon and
  `0x26` vibration; the parameters are zone masks and 3-bit strengths (see
  `Padwave.Output.TriggerEffects`, which builds them). Enabled by `valid_flag0`
  bits `:right_trigger` and `:left_trigger`.
  """
  def trigger_fields(side, mode, params \\ []) when side in [:left, :right] do
    case side do
      :right ->
        %{
          valid_flag0: [:right_trigger],
          right_trigger_mode: mode,
          right_trigger_params: trigger_params(params)
        }

      :left ->
        %{
          valid_flag0: [:left_trigger],
          left_trigger_mode: mode,
          left_trigger_params: trigger_params(params)
        }
    end
  end

  defp trigger_params(params) do
    params
    |> Enum.take(10)
    |> then(&(&1 ++ List.duplicate(0, 10 - length(&1))))
    |> :erlang.list_to_binary()
  end
end
