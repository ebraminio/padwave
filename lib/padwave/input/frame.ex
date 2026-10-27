defmodule Padwave.Input.Frame do
  @moduledoc """
  One decoded controller report, in the shape the rest of the app wants:
  named buttons, named d-pad direction, battery, headset jack state.
  """

  import Bitwise



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
  Builds a frame from one `padwave input` line. The CLI has already validated
  the CRC and decoded the fields; this only puts them in the shape the mapper
  and the panel expect.
  """
  def from_line("frame " <> rest) do
    f =
      rest
      |> String.split(" ", trim: true)
      |> Map.new(fn pair ->
        [key, value] = String.split(pair, "=", parts: 2)
        {key, value}
      end)

    buttons =
      f["buttons"]
      |> String.split(",", trim: true)
      |> Enum.map(&String.to_atom/1)

    fields = %{
      accel_x: int(f["ax"]),
      accel_y: int(f["ay"]),
      accel_z: int(f["az"]),
      gyro_x: int(f["gx"]),
      gyro_y: int(f["gy"]),
      gyro_z: int(f["gz"]),
      trigger_feedback_l: int(f["tfl"]),
      trigger_feedback_r: int(f["tfr"])
    }

    %__MODULE__{
      at: System.monotonic_time(:millisecond),
      seq: int(f["seq"]),
      lx: int(f["lx"]),
      ly: int(f["ly"]),
      rx: int(f["rx"]),
      ry: int(f["ry"]),
      l2: int(f["l2"]),
      r2: int(f["r2"]),
      dpad: String.to_atom(f["dpad"]),
      buttons: buttons,
      battery: int(f["battery"]),
      charging: charging_nibble(int(f["charging"])),
      jack: f["jack"] == "1",
      mic: f["mic"] == "1",
      mute: :mute in buttons,
      temperature: int(f["temp"]),
      g_force: g_force(fields),
      touches: [touch_hex(f["t1"]), touch_hex(f["t2"])],
      raw: decode_hex(f["raw"]),
      fields: fields
    }
  end

  def from_line(_other), do: nil

  defp int(nil), do: 0
  defp int(value), do: String.to_integer(value)

  defp charging_nibble(0), do: :discharging
  defp charging_nibble(1), do: :charging
  defp charging_nibble(2), do: :full
  defp charging_nibble(_), do: :unknown

  defp decode_hex(nil), do: <<>>

  defp decode_hex(hex) do
    case Base.decode16(hex, case: :mixed) do
      {:ok, bin} -> bin
      _ -> <<>>
    end
  end

  defp touch_hex(nil), do: touch(<<0, 0, 0, 0>>)

  defp touch_hex(hex) do
    case Base.decode16(hex, case: :mixed) do
      {:ok, <<_::binary-size(4)>> = bytes} -> touch(bytes)
      _ -> touch(<<0, 0, 0, 0>>)
    end
  end

  def pressed?(%__MODULE__{buttons: buttons}, button), do: button in buttons

  def pressed?(%__MODULE__{dpad: dpad}, dpad), do: true
  def pressed?(_frame, _button), do: false

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

end
