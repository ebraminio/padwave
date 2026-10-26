defmodule Padwave.Input.Frame do
  @moduledoc """
  One decoded controller report, in the shape the rest of the app wants:
  named buttons, named d-pad direction, battery, headset jack state.
  """

  import Bitwise

  alias Padwave.Config
  alias Padwave.Input.Crc

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
    with :ok <- check_signature(bin, spec),
         fields <- apply(Padwave.Schema.DualSenseInput, :"decode_#{spec}", [bin]) do
      case fields do
        {:error, reason} -> {:error, reason}
        fields -> {:ok, from_fields(fields, bin)}
      end
    end
  end

  # Bluetooth reports are signed; USB reports are not. A corrupted report must
  # never become a button press.
  defp check_signature(bin, :input) do
    if Crc.valid_input?(bin), do: :ok, else: {:error, :bad_crc}
  end

  defp check_signature(_bin, _spec), do: :ok

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
      dpad: frame_dpad(fields.buttons.dpad),
      buttons: pressed_buttons(fields),
      battery: battery(fields.status0),
      charging: charging(fields.status0),
      jack: fields.status1.headset,
      mic: fields.status1.microphone,
      mute: fields.buttons.mute,
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
  True when something a human did changed since `prev` — buttons, d-pad, headset
  or triggers. This is the reliable half of the signal.
  """
  def real_activity?(nil, _frame), do: false

  def real_activity?(prev, frame) do
    prev.buttons != frame.buttons or
      prev.dpad != frame.dpad or
      prev.jack != frame.jack or
      abs(prev.l2 - frame.l2) > 10 or
      abs(prev.r2 - frame.r2) > 10
  end

  @doc """
  True when a stick moved further than `:analog_wake_delta` counts since `prev`.

  Sticks are worn and jitter by a count or two, so this is thresholded; the
  mapper additionally caps how many times analogue movement alone may reset the
  sleep timer, so a drifting stick cannot keep the radio awake all night.
  """
  def analog_activity?(nil, _frame), do: false

  def analog_activity?(prev, frame) do
    case Config.get(:analog_wake_delta, 24) do
      delta when is_integer(delta) and delta > 0 ->
        abs(prev.lx - frame.lx) > delta or abs(prev.ly - frame.ly) > delta or
          abs(prev.rx - frame.rx) > delta or abs(prev.ry - frame.ry) > delta

      _ ->
        false
    end
  end

  @doc "Either kind of activity."
  def activity?(prev, frame), do: real_activity?(prev, frame) or analog_activity?(prev, frame)

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

  defp frame_dpad(value) when is_atom(value), do: value
  defp frame_dpad(_value), do: :unknown

  defp pressed_buttons(fields) do
    [
      square: fields.buttons.square,
      cross: fields.buttons.cross,
      circle: fields.buttons.circle,
      triangle: fields.buttons.triangle,
      l1: fields.buttons.l1,
      r1: fields.buttons.r1,
      l2: fields.buttons.l2,
      r2: fields.buttons.r2,
      create: fields.buttons.create,
      options: fields.buttons.options,
      l3: fields.buttons.l3,
      r3: fields.buttons.r3,
      ps: fields.buttons.ps,
      touchpad: fields.buttons.touchpad,
      mute: fields.buttons.mute,
      fn1: fields.buttons.fn1,
      fn2: fields.buttons.fn2,
      left_paddle: fields.buttons.left_paddle,
      right_paddle: fields.buttons.right_paddle
    ]
    |> Enum.filter(fn {_button, pressed?} -> pressed? end)
    |> Enum.map(fn {button, _} -> button end)
  end
end
