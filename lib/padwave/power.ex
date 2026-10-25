defmodule Padwave.Power do
  @moduledoc """
  Switches the controller off, and reports what BlueZ thinks about it.

  A DualSense is "off" the moment its Bluetooth link is dropped; it does not
  accept a HID power-down command over this transport. `bluetoothctl
  disconnect` is the polite, reversible way to do it: the controller goes to
  sleep and reconnects on the PS button.
  """

  require Logger

  alias Padwave.Input.Device

  @doc "Disconnects the controller. Returns `:ok` or `{:error, reason}`."
  def disconnect do
    case mac() do
      nil ->
        {:error, :no_controller}

      mac ->
        Logger.info("[power] disconnecting #{mac}")

        case System.cmd("bluetoothctl", ["disconnect", mac], stderr_to_stdout: true) do
          {output, 0} -> {:ok, String.trim(output)}
          {output, code} -> {:error, {code, String.trim(output)}}
        end
    end
  end

  def mac do
    case Device.info() do
      %{mac: mac} when is_binary(mac) and mac != "" -> mac
      _ -> nil
    end
  rescue
    _ -> nil
  end

  def connected?, do: Device.connected?()

  @doc "Battery percentage, from the last decoded frame."
  def battery do
    case Padwave.Input.Mapper.snapshot() do
      %{frame: %{battery: battery, charging: charging}} when is_integer(battery) ->
        %{percent: battery, charging: charging}

      _ ->
        nil
    end
  rescue
    _ -> nil
  end
end
