defmodule Padwave.Schema.DualSenseFeature do
  @moduledoc """
  DualSense feature reports — the configuration channel.

  Feature reports are read (and occasionally written) with a HID `GET_REPORT` /
  `SET_REPORT` on report ids that are not part of the streaming protocol. The
  kernel uses three of them and the field offsets below are taken from
  `hid-playstation.c`; DualSenseClient's descriptor notes corroborate the ids
  and lengths (`0x05` 41, `0x09` 20, `0x20` 64).

  Feature reports are signed with CRC-32 seed `0xA3` over Bluetooth (the input
  stream uses `0xA1`, the output stream `0xA2`). padwave does not read them —
  the kernel already exposes the results (MAC, battery, calibration) — but they
  are described so the whole protocol is visible.

  Known ids:

  | id | length | what |
  |----|--------|------|
  | `0x05` | 41 | motion-sensor calibration: 17 int16 LE at `buf[1..34]` |
  | `0x09` | 20 | pairing info; the MAC is `buf[1..6]` (some clients read it reversed) |
  | `0x20` | 64 | firmware/hardware info: hw `buf[24]`, fw `buf[28]`, update `buf[44]` |
  | `0x80` | — | factory command write (Edge/vendor tooling) |
  | `0x81` | 64 | the matching command response |
  | `0x65` | 64 | DualSense Edge factory handshake |

  The Edge also exposes profile reports in the `0x60..0x7B` range; those are
  well beyond what padwave needs.
  """

  use Padwave.Wire

  packet :calibration, 0x05, size: 41, doc: "Motion-sensor calibration" do
    i16le :gyro_pitch_bias       # 0x01..0x22  seventeen little-endian int16:
    i16le :gyro_yaw_bias         #             three gyro biases, then plus/minus
    i16le :gyro_roll_bias        #             at 2× speed for each axis, then
    i16le :gyro_pitch_plus       #             gyro speed limits, then accel
    i16le :gyro_pitch_minus      #             plus/minus at 1 g per axis
    i16le :gyro_yaw_plus
    i16le :gyro_yaw_minus
    i16le :gyro_roll_plus
    i16le :gyro_roll_minus
    i16le :gyro_speed_plus
    i16le :gyro_speed_minus
    i16le :acc_x_plus
    i16le :acc_x_minus
    i16le :acc_y_plus
    i16le :acc_y_minus
    i16le :acc_z_plus
    i16le :acc_z_minus
    bytes :unknown_35, 6         # 0x23..0x28  not used by the kernel
  end

  packet :pairing, 0x09, size: 20, doc: "Bluetooth pairing info" do
    bytes :mac, 6                # 0x01..0x06  the controller's own address
    bytes :unknown_07, 13        # 0x07..0x13
  end

  packet :firmware, 0x20, size: 64, doc: "Firmware and hardware information" do
    bytes :unknown_01, 23        # 0x01..0x17
    u32le :hw_version            # 0x18..0x1b
    u32le :fw_version            # 0x1c..0x1f
    bytes :unknown_32, 12        # 0x20..0x2b
    u16le :update_version        # 0x2c..0x2d  feature/PCB revision, varies by model
    bytes :unknown_46, 18        # 0x2e..0x3f
  end
end
