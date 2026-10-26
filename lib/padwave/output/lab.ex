defmodule Padwave.Output.Lab do
  @moduledoc """
  A small door into the output report, for the web UI's playground.

  Everything here goes through the same `Padwave.Output.Hid` writer as the
  lightbar and trigger effects, so a report built from arbitrary fields is
  framed and signed exactly like the ones padwave sends itself.

  Kinds accepted by `apply/1`:

    * `lightbar` — `%{"rgb" => [r, g, b]}` override, until reset
    * `lightbar_reset` — back to the state-driven colour
    * `player_leds` — `%{"mask" => 0..31}`
    * `player_leds_reset` — back to whatever the kernel has set
    * `trigger` — `%{"side" => "left"|"right", "mode" => n, "params" => [..], "ms" => n}`
    * `fields` — `%{"fields" => %{field_name => value}}`, sent verbatim
  """

  import Bitwise, only: [band: 2]

  alias Padwave.Output.{Hid, Lightbar, TriggerEffects}

  # High-level effects keep the panel honest: the UI sends "vibration with
  # position/amplitude/frequency", not a mode byte and ten opaque bytes.
  defp effect(%{"effect" => "vibration"} = p) do
    TriggerEffects.vibration(
      to_int(p["position"], 2),
      to_int(p["amplitude"], 6),
      to_int(p["frequency"], 25)
    )
  end

  defp effect(%{"effect" => "feedback"} = p) do
    TriggerEffects.feedback(to_int(p["position"], 2), to_int(p["amplitude"], 6))
  end

  defp effect(%{"effect" => "weapon"} = p) do
    TriggerEffects.weapon(
      to_int(p["position"], 2),
      to_int(p["stop"], 6),
      to_int(p["amplitude"], 6)
    )
  end

  defp effect(%{"effect" => "off"}), do: TriggerEffects.off()

  defp effect(p) do
    mode = p["mode"] |> to_int(0x05) |> clamp()
    params = p["params"] |> List.wrap() |> Enum.map(&to_int(&1, 0)) |> Enum.map(&clamp/1)
    {mode, params |> Enum.take(10) |> Enum.map(&<<&1>>) |> Enum.join() |> pad_params()}
  end

  defp pad_params(bytes), do: bytes <> :binary.copy(<<0>>, max(10 - byte_size(bytes), 0))

  def apply(%{"kind" => "lightbar", "rgb" => [r, g, b]}) do
    Lightbar.set_rgb(clamp(r), clamp(g), clamp(b))
    {:ok, "lightbar overridden"}
  end

  # valid_flag2 bit 1 + lightbar_setup bit 1 = the kernel's "fade light out".
  def apply(%{"kind" => "lightbar_fade"}) do
    Hid.send_fields(%{"valid_flag2" => ["lightbar_setup"], "lightbar_setup" => 0x02})
    {:ok, "lightbar fade-out sent"}
  end

  # valid_flag2 bit 0 = brightness, then the value in led_brightness.
  def apply(%{"kind" => "lightbar_brightness", "value" => value}) do
    Hid.send_fields(%{
      "valid_flag2" => ["brightness"],
      "led_brightness" => value |> to_int(255) |> clamp()
    })

    {:ok, "lightbar brightness set"}
  end

  def apply(%{"kind" => "lightbar_reset"}) do
    Lightbar.clear()
    {:ok, "lightbar back to state colours"}
  end

  def apply(%{"kind" => "player_leds", "mask" => mask}) when is_integer(mask) do
    Hid.player_leds(band(mask, 0x1F))
    {:ok, "player LEDs set"}
  end

  def apply(%{"kind" => "player_leds_reset"}) do
    Hid.reset_player_leds()
    {:ok, "player LEDs restored"}
  end

  def apply(%{"kind" => "trigger"} = params) do
    side = if params["side"] == "left", do: :left, else: :right
    {mode, bytes} = effect(params)

    case to_int(params["ms"], 800) do
      ms when ms > 0 -> Padwave.Output.Triggers.test(side, ms, mode, bytes)
      _ -> Hid.trigger(side, mode, bytes)
    end

    {:ok, "trigger #{side} #{TriggerEffects.name(mode)}"}
  end

  def apply(%{"kind" => "fields", "fields" => fields}) when is_map(fields) do
    Hid.send_fields(fields)
    {:ok, "report sent"}
  rescue
    error -> {:error, Exception.message(error)}
  end

  def apply(_params), do: {:error, "unknown kind"}

  defp clamp(value) when is_integer(value), do: value |> max(0) |> min(255)
  defp clamp(_value), do: 0

  defp to_int(value, _default) when is_integer(value), do: value

  defp to_int(value, default) when is_binary(value) do
    case Integer.parse(value) do
      {number, _} -> number
      :error -> default
    end
  end

  defp to_int(_value, default), do: default
end
