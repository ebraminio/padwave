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

  alias Padwave.Output.{Audio, Hid, Lightbar, TriggerEffects}

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

  def apply(%{"kind" => "mic_monitor"} = params) do
    enabled = params["enabled"] == true
    :ok = Padwave.Mic.set_monitor(enabled)
    {:ok, "input level #{if enabled, do: "on", else: "off"}"}
  end

  def apply(%{"kind" => "lightbar", "rgb" => [r, g, b]}) do
    Lightbar.set_rgb(clamp(r), clamp(g), clamp(b))
    {:ok, "lightbar overridden"}
  end

  # A 0x36 packet built by padwave itself, carrying Opus noise. Only meaningful
  # while the audio service is stopped: its own 0x36 would overwrite ours.
  def apply(%{"kind" => "audio_noise"} = params) do
    ms = params["ms"] |> to_int(500) |> max(10) |> min(10_000)
    level = params["level"] |> to_int(3_000) |> max(0) |> min(30_000)
    output = if params["output"] == "speaker", do: :speaker, else: :jack
    Audio.noise(ms, level: level, output: output)
  end

  # Drive the selected playback engine directly, bypassing the radio. Used for
  # the mpv-vs-ffmpeg comparisons: same file, same capture, only the engine
  # differs.
  def apply(%{"kind" => "engine_play", "path" => path}) when is_binary(path) do
    if File.regular?(path) do
      Padwave.Player.Native.play(path, start: 0, fade: 0)
      {:ok, "engine playing #{path}"}
    else
      {:error, "no such file"}
    end
  end

  def apply(%{"kind" => "engine_stop"}) do
    Padwave.Player.Native.stop(fade: 0)
    {:ok, "engine stopped"}
  end

  # The prototype playback path: ffmpeg decodes the current station, our encoder
  # and 0x36 builder do the rest. Stop the radio and the audio service first.
  def apply(%{"kind" => "audio_file"} = params) do
    case Padwave.Radio.status() do
      %{current: %{path: path}} when is_binary(path) ->
        ms = params["ms"] |> to_int(5_000) |> max(100) |> min(120_000)
        output = if params["output"] == "speaker", do: :speaker, else: :jack
        Audio.play_file(path, limit_ms: ms, output: output)

      _ ->
        {:error, "no current station"}
    end
  end

  # Classic rumble motors, claimed with valid_flag0 bit 0. The kernel drives
  # these too for a game's force feedback, so our values last until the next FF
  # event — fine for poking at the hardware, not a persistent setting. A zero
  # report after `ms` stops them.
  def apply(%{"kind" => "rumble"} = params) do
    left = params["left"] |> to_int(0) |> clamp()
    right = params["right"] |> to_int(0) |> clamp()
    ms = params["ms"] |> to_int(400) |> max(0) |> min(5_000)

    Hid.send_fields(%{"valid_flag0" => ["rumble"], "motor_left" => left, "motor_right" => right})

    if ms > 0 do
      Task.start(fn ->
        Process.sleep(ms)
        Hid.send_fields(%{"valid_flag0" => ["rumble"], "motor_left" => 0, "motor_right" => 0})
      end)
    end

    {:ok, "rumble #{left}/#{right} for #{ms} ms"}
  end

  # Eight toggles for power_save_control. Only bit 4 (mic mute) and bit 6
  # (audio mute) are documented; the rest are for finding out.
  def apply(%{"kind" => "power_save"} = params) do
    bits = params["bits"] |> List.wrap() |> Enum.map(&to_int(&1, 0)) |> Enum.map(&band(&1, 7))
    value = Enum.reduce(bits, 0, fn bit, acc -> Bitwise.bor(acc, Bitwise.bsl(1, bit)) end)

    if value == 0 do
      # Clear the device once, then stop claiming it in every report.
      Hid.set_power_save(nil)
      Hid.send_fields(%{"valid_flag1" => ["power_save"], "power_save_control" => 0})
      {:ok, "power_save cleared"}
    else
      Hid.set_power_save(value)
      {:ok, "power_save_control = #{value} (now in every report)"}
    end
  end

  # valid_flag2 bit 1 ("color_fade") + lightbar_setup bit 1 = "fade light out".
  def apply(%{"kind" => "lightbar_fade"}) do
    Lightbar.fade_out()
    {:ok, "lightbar fade-out sent"}
  end

  def apply(%{"kind" => "lightbar_setup", "value" => value}) do
    Lightbar.set_setup(value |> to_int(0) |> clamp())
    {:ok, "lightbar_setup set"}
  end

  # valid_flag2 bit 0 = brightness; the bar repaints with the current colour.
  def apply(%{"kind" => "lightbar_brightness", "value" => value}) do
    Lightbar.set_brightness(value |> to_int(255) |> clamp())
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
