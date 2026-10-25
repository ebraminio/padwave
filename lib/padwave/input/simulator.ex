defmodule Padwave.Input.Simulator do
  @moduledoc """
  Injects synthetic 0x31 reports into the mapper.

  This exists so the button layout can be exercised end-to-end — from packet
  bits to a station change — without a human holding the controller. It is
  what `/api/debug/frame` uses, and it is also how the tests drive the mapper.

  While a simulated press is in flight the real device's frames are ignored by
  the mapper, otherwise a "hold" would be interrupted by the controller's own
  250 Hz stream and look like a rapid series of taps.
  """

  import Bitwise, only: [band: 2, bor: 2, bnot: 1, bsl: 2]

  alias Padwave.Input.{Frame, Mapper}

  @bit_positions %{
    {9, :square} => 4,
    {9, :cross} => 5,
    {9, :circle} => 6,
    {9, :triangle} => 7,
    {10, :l1} => 0,
    {10, :r1} => 1,
    {10, :l2} => 2,
    {10, :r2} => 3,
    {10, :create} => 4,
    {10, :options} => 5,
    {10, :l3} => 6,
    {10, :r3} => 7,
    {11, :ps} => 0,
    {11, :touchpad} => 1,
    {11, :mute} => 2
  }

  @by_name Map.new(@bit_positions, fn {{byte, name}, position} -> {name, {byte, position}} end)

  @dpad %{
    up: 0,
    up_right: 1,
    right: 2,
    down_right: 3,
    down: 4,
    down_left: 5,
    left: 6,
    up_left: 7,
    none: 8
  }

  @doc """
  Presses `buttons` (and optionally a d-pad direction / analogue trigger
  values) for `duration_ms`, then releases. Returns immediately.
  """
  def press(patch, duration_ms \\ 140) when is_map(patch) do
    spawn(fn -> run(patch, duration_ms) end)
  end

  @doc "Like `press/2` but blocks until the press (and its release) is done."
  def press!(patch, duration_ms \\ 140) do
    pid = spawn(fn -> run(patch, duration_ms) end)
    ref = Process.monitor(pid)

    receive do
      {:DOWN, ^ref, :process, ^pid, _reason} -> :ok
    after
      duration_ms + 10_000 -> :ok
    end
  end

  defp run(patch, duration_ms) do
    duration_ms = duration_ms |> max(0) |> min(10_000)
    raw = build(patch)

    with {:ok, held} <- Frame.from_report(raw),
         {:ok, released} <- Frame.from_report(release(raw)) do
      send(Mapper, {:sim_begin})
      hold(held, System.monotonic_time(:millisecond) + duration_ms)
      send(Mapper, {:sim_frame, released})
      Process.sleep(80)
      send(Mapper, {:sim_end})
    end

    :ok
  end

  @doc "Builds a full 78-byte report with `patch` applied to the last real frame."
  def build(patch) do
    base = base_frame()

    buttons = Map.get(patch, :buttons, Map.get(patch, "buttons", []))
    dpad = patch |> Map.get(:dpad, Map.get(patch, "dpad", :none)) |> normalize_dpad()
    l2 = Map.get(patch, :l2, Map.get(patch, "l2", 0))
    r2 = Map.get(patch, :r2, Map.get(patch, "r2", 0))

    base
    |> put_byte(6, l2)
    |> put_byte(7, r2)
    |> apply_dpad(dpad)
    |> apply_buttons(buttons)
  end

  def to_hex(frame), do: Base.encode16(frame, case: :lower)

  # -- internals -------------------------------------------------------------

  defp hold(frame, deadline) do
    send(Mapper, {:sim_frame, frame})

    if System.monotonic_time(:millisecond) < deadline do
      Process.sleep(25)
      hold(frame, deadline)
    else
      :ok
    end
  end

  defp base_frame do
    case Mapper.snapshot() do
      %{frame: %{raw: raw}} when is_binary(raw) and byte_size(raw) == 78 ->
        raw

      _ ->
        <<0x31, 0xD1>> <>
          <<128, 128, 128, 128, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0>> <>
          :binary.copy(<<0>>, 60)
    end
  end

  defp release(frame) do
    frame
    |> put_byte(6, 0)
    |> put_byte(7, 0)
    |> apply_dpad(:none)
    |> clear_buttons()
  end

  defp clear_buttons(frame) do
    Enum.reduce(@by_name, frame, fn {name, _pos}, acc -> set_bit(acc, name, false) end)
  end

  defp apply_dpad(frame, direction) do
    value = Map.get(@dpad, direction, 8)
    current = :binary.at(frame, 9)
    put_byte(frame, 9, bor(band(current, 0xF0), value))
  end

  defp apply_buttons(frame, buttons) do
    Enum.reduce(buttons, frame, fn button, acc -> set_bit(acc, normalize(button), true) end)
  end

  defp set_bit(frame, name, true) do
    case Map.fetch(@by_name, name) do
      {:ok, {byte, position}} ->
        put_byte(frame, byte, bor(:binary.at(frame, byte), bsl(1, position)))

      :error ->
        frame
    end
  end

  defp set_bit(frame, name, false) do
    case Map.fetch(@by_name, name) do
      {:ok, {byte, position}} ->
        put_byte(frame, byte, band(:binary.at(frame, byte), bnot(bsl(1, position))))

      :error ->
        frame
    end
  end

  defp normalize_dpad(direction) when is_binary(direction), do: String.to_existing_atom(direction)
  defp normalize_dpad(direction) when is_atom(direction), do: direction

  defp normalize(button) when is_atom(button), do: button
  defp normalize(button) when is_binary(button), do: String.to_existing_atom(button)

  defp put_byte(frame, index, value) when value in 0..255 do
    <<head::binary-size(index), _old, tail::binary>> = frame
    <<head::binary, value, tail::binary>>
  end
end
