defmodule Padwave.Input.Frame do
  @moduledoc """
  One decoded controller report, in the shape the rest of the app wants:
  named buttons, named d-pad direction, battery, headset jack state.
  """

  import Bitwise

  alias Padwave.Config

  @dpad %{
    0 => :up,
    1 => :up_right,
    2 => :right,
    3 => :down_right,
    4 => :down,
    5 => :down_left,
    6 => :left,
    7 => :up_left,
    8 => :none
  }

  defstruct [
    :at,
    :seq,
    :lx,
    :ly,
    :rx,
    :ry,
    :l2,
    :r2,
    :dpad,
    :buttons,
    :battery,
    :charging,
    :jack,
    :mic,
    :mute,
    :temperature,
    :g_force,
    :touches,
    :raw,
    :fields
  ]

  @type t :: %__MODULE__{}

  @doc """
  Decodes a raw report. `spec` is `:input` for Bluetooth or `:input_usb` for
  USB; the payload is the same either way, only the framing differs.
  """
  def from_report(bin, spec \\ :input) when is_binary(bin) do
    case apply(Padwave.Schema.DualSenseInput, :"decode_#{spec}", [bin]) do
      {:error, reason} -> {:error, reason}
      fields -> {:ok, from_fields(fields, bin)}
    end
  end

  def from_fields(fields, bin) do
    %__MODULE__{
      at: System.monotonic_time(:millisecond),
      seq: fields.seq,
      lx: fields.lx,
      ly: fields.ly,
      rx: fields.rx,
      ry: fields.ry,
      l2: fields.l2,
      r2: fields.r2,
      dpad: Map.get(@dpad, fields.buttons0.dpad, :unknown),
      buttons: pressed_buttons(fields),
      battery: battery(fields.status0),
      charging: charging(fields.status0),
      jack: (fields.status1 &&& 1) == 1,
      mic: (fields.status1 &&& 2) == 2,
      mute: fields.buttons2.mute,
      temperature: fields.temperature,
      g_force: g_force(fields),
      touches: [touch(fields.touch1), touch(fields.touch2)],
      raw: bin,
      fields: fields
    }
  end

  def pressed?(%__MODULE__{buttons: buttons}, button), do: button in buttons

  def pressed?(%__MODULE__{dpad: dpad}, dpad), do: true
  def pressed?(_frame, _button), do: false

  @doc """
  True when something a human did changed since `prev`.

  Analogue sticks are deliberately excluded by default: the sticks on this
  controller are worn and jitter even when untouched, and a jittering stick
  must not keep the radio awake forever. Set `:analog_wake_delta` to enable
  them anyway.
  """
  def activity?(nil, _frame), do: false

  def activity?(prev, frame) do
    prev.buttons != frame.buttons or
      prev.dpad != frame.dpad or
      prev.jack != frame.jack or
      abs(prev.l2 - frame.l2) > 10 or
      abs(prev.r2 - frame.r2) > 10 or
      sticks_moved?(prev, frame)
  end

  defp sticks_moved?(prev, frame) do
    case Config.get(:analog_wake_delta, 0) do
      delta when is_integer(delta) and delta > 0 ->
        abs(prev.lx - frame.lx) > delta or abs(prev.ly - frame.ly) > delta or
          abs(prev.rx - frame.rx) > delta or abs(prev.ry - frame.ry) > delta

      _ ->
        false
    end
  end

  @doc "Battery percentage, 5..100, matching the kernel's power_supply value."
  def battery(status0) do
    level = status0 &&& 0x0F
    min(level * 10 + 5, 100)
  end

  def charging(status0) do
    case status0 >>> 4 do
      0 -> :discharging
      1 -> :charging
      2 -> :full
      _ -> :unknown
    end
  end

  @doc """
  Total acceleration in g, gravity included — the same quantity AntennaPod's
  shake listener compares against 2.25 g. The DualSense reports roughly 8192
  counts per g.
  """
  def g_force(fields) do
    magnitude =
      :math.sqrt(
        fields.accel_x * fields.accel_x +
          fields.accel_y * fields.accel_y +
          fields.accel_z * fields.accel_z
      )

    Float.round(magnitude / 8192, 3)
  end

  @doc """
  A touch point is four bytes: contact/id, x low, four bits of x high and four
  bits of y low, then y high. Bit 7 of the contact byte means "not touching".
  """
  def touch(<<contact, x_lo, packed, y_hi>>) do
    %{
      active: (contact &&& 0x80) == 0,
      id: contact &&& 0x7F,
      x: (packed &&& 0x0F) <<< 8 ||| x_lo,
      y: y_hi <<< 4 ||| packed >>> 4
    }
  end

  def touch(_other), do: %{active: false, id: 0, x: 0, y: 0}

  defp pressed_buttons(fields) do
    [
      square: fields.buttons0.square,
      cross: fields.buttons0.cross,
      circle: fields.buttons0.circle,
      triangle: fields.buttons0.triangle,
      l1: fields.buttons1.l1,
      r1: fields.buttons1.r1,
      l2: fields.buttons1.l2,
      r2: fields.buttons1.r2,
      create: fields.buttons1.create,
      options: fields.buttons1.options,
      l3: fields.buttons1.l3,
      r3: fields.buttons1.r3,
      ps: fields.buttons2.ps,
      touchpad: fields.buttons2.touchpad,
      mute: fields.buttons2.mute
    ]
    |> Enum.filter(fn {_button, pressed?} -> pressed? end)
    |> Enum.map(fn {button, _} -> button end)
  end
end
