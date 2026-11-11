// dualsense-extra.hh — command, authentication and USB-topology metadata.

#pragma once
#include "dualsense.hh"

namespace dualsense {
namespace internal {

// psdevwiki HID command metadata. Command report IDs are part of ReportId above;
// this table is metadata only; it does not construct payloads or perform I/O.

enum class CommandDevice : uint8_t {
  None       = 0,
  MainMcu    = 1,
  Gyro       = 2,
  Nvs        = 3,
  TouchPanel = 5,
  Audio      = 6, // audio codec command family; same ordinal as VenomFw
  VenomFw    = 6,
  Stick      = 12,
  DeviceInfo = 9,
  Bluetooth  = 14,
  Venom      = 15,
  Codec      = 16,
  Vdd        = 17,
};

static constexpr uint8_t kNoCommandAction = 0xFF;

struct CommandInfo {
  const char *name;
  ReportId report;
  CommandDevice device;
  uint8_t action;
  uint8_t dataLen;
  bool dangerous;
  const char *extra;
};

// ReportIDs, DeviceIDs and Actions from the psdevwiki command dump. `extra`
// records fields whose interpretation is not part of the generic command
// header; keep it as the place to add payload details.
// Observed on a retail Bluetooth DualSense: the feature descriptor exposes
// 0x05, 0x08, 0x09, 0x0b, 0x20, 0x22, 0x80..0x83 and 0xF0..0xF7.
// 0x90/0x91/0xA0/0xA4/0xA5 are not feature report IDs there; writes to the
// 0x80 feature report return EIO. USB may expose a different command surface.
inline constexpr CommandInfo kCommandTable[] = {
    {"Get MCU Unique ID", ReportId::CommandHost, CommandDevice::MainMcu, 9, 0, false,
     "MediaTek main MCU unique ID"},
    {"Read Device Info", ReportId::CommandHost, CommandDevice::MainMcu, 12, 0, false, ""},
    {"Erase Device Info", ReportId::CommandHost, CommandDevice::MainMcu, 13, 0, true, "DANGEROUS"},
    {"Spider DSP Init Get Status", ReportId::CommandStatus, CommandDevice::Codec, 1, 0, false,
     "status query"},
    {"Spider DSP Get Status", ReportId::CommandStatus, CommandDevice::Codec, 2, 0, false,
     "status query"},
    {"Spider DSP Patch Finalize", ReportId::CommandHost, CommandDevice::Codec, 5, 0, false, ""},
    {"Spider DSP Finalize Get Status", ReportId::CommandStatus, CommandDevice::Codec, 5, 0, false,
     "status query"},
    {"Spider DSP Firmware Patch", ReportId::CommandHost, CommandDevice::Codec, 2, 57, false, ""},
    {"Spider DSP Firmware Patch Init", ReportId::CommandHost, CommandDevice::Codec, 1, 0, false,
     ""},
    {"Spider DSP Firmware Verify", ReportId::CommandHost, CommandDevice::Codec, 3, 0, false, ""},
    {"Spider DSP Firmware Verify Status", ReportId::CommandStatus, CommandDevice::Codec, 5, 0,
     false, "status query"},
    {"Spider DSP Firmware Verify Request", ReportId::CommandHost, CommandDevice::Codec, 7, 0, false,
     ""},
    {"Spider DSP Firmware Verify Request Status", ReportId::CommandStatus, CommandDevice::Codec, 7,
     0, false, "status query"},
    {"Get TP UID", ReportId::CommandHost, CommandDevice::TouchPanel, 2, 0, false,
     "touch panel unique ID"},
    {"Get TP Version", ReportId::CommandHost, CommandDevice::TouchPanel, 4, 0, false, ""},
    {"Get Table ID", ReportId::CommandQuery, CommandDevice::Nvs, 2, 5, false, "DataLenLocal = 5"},
    {"Reset", ReportId::CommandHost, CommandDevice::MainMcu, 1, 0, false, ""},
    {"Request Table ID", ReportId::CommandCodec, CommandDevice::Nvs, 2, 0, false, ""},
    {"Reboot Secure Boot Loader", ReportId::CommandHost, CommandDevice::None, kNoCommandAction, 0,
     false, "RebootCmd = 1"},
    {"Read Patch Info", ReportId::CommandHost, CommandDevice::DeviceInfo, 13, 0, false, ""},
    {"Start Gyroscope Offset", ReportId::CommandGyro, CommandDevice::Gyro, 1, 0, false,
     "TargetID = 7"},
    {"Get Gyroscope Offset State", ReportId::CommandGyroStatus, CommandDevice::Gyro,
     kNoCommandAction, 0, false, "TargetID = 7"},
    {"Get BlueTooth Patch Data", ReportId::CommandStatus, CommandDevice::Bluetooth, 3, 0, false,
     "status query"},
    {"Get BlueTooth Patch Info", ReportId::CommandStatus, CommandDevice::DeviceInfo, 13, 0, false,
     "status query"},
    {"Get Audio Codec State", ReportId::CommandQuery, CommandDevice::Codec, 1, 0, false, ""},
    {"Flash", ReportId::CommandFlash, CommandDevice::None, kNoCommandAction, 57, false,
     "DownloadCmd = 0; WriteCmd = 1"},
    {"Audio Codec Select", ReportId::CommandCodec, CommandDevice::Codec, 1, 0, false, ""},
    {"Blue Tooth Patch", ReportId::CommandHost, CommandDevice::Bluetooth, 2, 57, false, ""},
    {"Read Blue Tooth Patch", ReportId::CommandHost, CommandDevice::Bluetooth, 4, 0, false, ""},
    {"Read Blue Tooth Patch Info", ReportId::CommandStatus, CommandDevice::Bluetooth, 4, 0, false,
     "status query"},
    {"Blue Tooth Patch Finalize", ReportId::CommandHost, CommandDevice::Bluetooth, 5, 0, false, ""},
    {"Blue Tooth Patch Init", ReportId::CommandHost, CommandDevice::Bluetooth, 1, 0, false, ""},
    {"Blue Tooth Patch Restart", ReportId::CommandHost, CommandDevice::Bluetooth, 6, 0, false, ""},
    {"Blue Tooth Patch Verify", ReportId::CommandHost, CommandDevice::Bluetooth, 3, 0, false, ""},
    {"Blue Tooth Restart Result", ReportId::CommandStatus, CommandDevice::Bluetooth, 6, 0, false,
     "status query"},
    {"Get BT Address", ReportId::CommandStatus, CommandDevice::DeviceInfo, 2, 6, false,
     "status query"},
    {"Set BT Address", ReportId::CommandHost, CommandDevice::DeviceInfo, 1, 6, false,
     "ReadCmd = 2"},
    {"Set BlueTooth Disable", ReportId::CommandHost, CommandDevice::DeviceInfo, 5, 0, false,
     "Param = 0"},
    {"Set BlueTooth Enable", ReportId::CommandHost, CommandDevice::DeviceInfo, 5, 0, false,
     "Param = 1"},
    {"Set BlueTooth Table", ReportId::CommandTable, CommandDevice::None, kNoCommandAction, 16,
     false,
     "PowerTable = 1; RadioTable = 2; DataBegin = 0; PowerTableSize = 224; "
     "RadioTableSize = 112; DataMaxLen = 255; DataMinLen = 0"},
    {"Set DFU Mode", ReportId::CommandCodec, CommandDevice::None, kNoCommandAction, 0, false,
     "DVT/prototype only; DisableMode = 0; EnablePBLMode = 1; EnableSBLMode = 2; "
     "EnableMode = 1"},
    {"Set Hardware Version", ReportId::CommandHost, CommandDevice::MainMcu, 5, 0, false, ""},
    {"Write PCBAID", ReportId::CommandHost, CommandDevice::MainMcu, 3, 6, false, ""},
    {"Read PCBAID", ReportId::CommandHost, CommandDevice::MainMcu, 4, 0, false, ""},
    {"Set Power Charge Off", ReportId::CommandHost, CommandDevice::Gyro, 3, 0, false,
     "Param0 = 208; Param1 = 38; Param2 = 117; Param3 = 192"},
    {"Set Power Charge On", ReportId::CommandHost, CommandDevice::Gyro, 2, 0, false,
     "Param0 = 208; Param1 = 38; Param2 = 117; Param3 = 192"},
    {"Get Read OTP", ReportId::CommandQuery, CommandDevice::Nvs, 9, 0, false, ""},
    {"Set Read OTP", ReportId::CommandCodec, CommandDevice::Nvs, 9, 0, false,
     "Offset = 23; Length = 1"},
    {"Set Secure Boot Loader", ReportId::CommandCodec, CommandDevice::None, kNoCommandAction, 0,
     false, "SetCmd = 2"},
    {"Set VDD External Power Off", ReportId::CommandCodec, CommandDevice::Vdd, 1, 0, false,
     "Param = 0"},
    {"Set VDD External Power On", ReportId::CommandCodec, CommandDevice::Vdd, 1, 0, false,
     "Param = 1"},
    {"Venom Get Status", ReportId::CommandStatus, CommandDevice::Venom, 2, 0, false,
     "status query"},
    {"Set Venom FW Version", ReportId::CommandHost, CommandDevice::VenomFw, 3, 0, false, ""},
    {"Venom Finalize Get Status", ReportId::CommandStatus, CommandDevice::Venom, 5, 0, false,
     "status query"},
    {"Venom Firmware Patch", ReportId::CommandHost, CommandDevice::Venom, 2, 57, false, ""},
    {"Venom Firmware Patch Init", ReportId::CommandHost, CommandDevice::Venom, 1, 0, false, ""},
    {"Venom Firmware Verify", ReportId::CommandHost, CommandDevice::Venom, 3, 0, false, ""},
    {"Venom Firmware Verify Request", ReportId::CommandHost, CommandDevice::Venom, 7, 0, false, ""},
    {"Venom Firmware Verify Request Status", ReportId::CommandStatus, CommandDevice::Venom, 7, 0,
     false, "status query"},
    {"Venom Firmware Verify Status", ReportId::CommandStatus, CommandDevice::Venom, 5, 0, false,
     "status query"},
    {"Venom Init Get Status", ReportId::CommandStatus, CommandDevice::Venom, 1, 0, false,
     "status query"},
    {"Venom Patch Finalize", ReportId::CommandHost, CommandDevice::Venom, 5, 0, false, ""},
    {"Set bonding", ReportId::CommandHost, CommandDevice::MainMcu, 14, 0, false, ""},
    {"NVS Lock", ReportId::CommandHost, CommandDevice::Nvs, 1, 0, false, ""},
    {"NVS Unlock", ReportId::CommandHost, CommandDevice::Nvs, 2, 0, false,
     "Param0 = 101; Param1 = 50; Param2 = 64; Param3 = 12"},
    {"NVS Lock Status", ReportId::CommandHost, CommandDevice::Nvs, 3, 0, false,
     "0 = Unlocked; 1 = Locked"},
};

// Feature-report map captured from a PS5 <-> DualSense USB session
// (0xThreadz/dualsense-ps5-usb-research, fw 0x0110002a / hw 0x00000621).
// 0x02 is a GET report of 37 bytes in that descriptor, but was never exercised;
// it is omitted here because the same ID is the USB output report in this header.
// 0x08 SET was observed as 17 bytes with payload 08 11 00...; its purpose is
// unclear. 0x20 is a 64-byte config/calibration blob containing ASCII build
// date strings. The USB descriptor exposes 0x80/0x81 as 63-byte data reports,
// 0x82 with 9 bytes, 0x85 with 2 bytes and 0xA0 with 1 byte; 0x90, 0x91 and
// 0xA4 are absent. 0x80 is write-only through hidraw (GET returns EINVAL).
enum class FeatureAccess : uint8_t {
  Get,
  Set,
  GetSet,
};

struct FeatureReportInfo {
  ReportId id;
  uint8_t length;
  FeatureAccess access;
  const char *purpose;
};

inline constexpr FeatureReportInfo kFeatureReports[] = {
    {ReportId::FeatureCalibration, 41, FeatureAccess::Get, "motion calibration: 17 x int16 LE"},
    {ReportId::FeatureMode, 17, FeatureAccess::Set, "mode/feature negotiation"},
    {ReportId::FeaturePairing, 20, FeatureAccess::Get,
     "controller MAC at 1..6, host MAC at 10..15"},
    {ReportId::FeatureSetup, 42, FeatureAccess::Get, "initial setup"},
    {ReportId::FeatureFirmware, 64, FeatureAccess::Get, "config/firmware/calibration"},
    {ReportId::CommandHost, 64, FeatureAccess::GetSet, "host command"},
    {ReportId::CommandStatus, 64, FeatureAccess::Get, "host status"},
    {ReportId::Command82, 10, FeatureAccess::GetSet, "vendor feature"},
    {ReportId::Command83, 64, FeatureAccess::GetSet, "vendor feature"},
    {ReportId::Command84, 64, FeatureAccess::GetSet, "vendor feature"},
    {ReportId::Command85, 3, FeatureAccess::GetSet, "vendor feature"},
    {ReportId::CommandCodec, 2, FeatureAccess::GetSet, "codec/vendor feature"},
    {ReportId::AuthChallenge, 64, FeatureAccess::Set, "authentication challenge chunk"},
    {ReportId::AuthResponse, 0, FeatureAccess::Get, "authentication response; never observed"},
    {ReportId::AuthStatus, 16, FeatureAccess::Get, "authentication status poll"},
};

// USB authentication challenge, 64 bytes. Four chunks carry 56 payload bytes
// each (224 total). Byte 3 is the chunk index 0..3. The final four bytes appear
// to be a checksum over the 256-byte challenge; the algorithm is unknown, and
// the final chunk is zero-padded.
struct AuthenticationChallenge {
private:
  using DSUINT8  = internal::DSUINT8;
  using ReportId = internal::ReportId;

protected:
  ReportId reportIdValue; // 0x00  0xF0
  DSUINT8 type;           // 0x01  observed 0x01
  DSUINT8 subtype;        // 0x02  observed 0x01
  DSUINT8 chunk;          // 0x03  0..3
  DSUINT8 payload[56];    // 0x04..0x3b
  DSUINT8 checksum[4];    // 0x3c..0x3f  final chunk only; algorithm unknown

  static constexpr ReportId reportId = ReportId::AuthChallenge;
  static constexpr size_t size       = 64;
};
static_assert(sizeof(AuthenticationChallenge) == 64, "0xF0 must stay 64 bytes");

// 16-byte authentication status poll. Every observed response was identical:
// f2 00 01 11 followed by twelve zero bytes.
struct AuthenticationStatus {
  using DSUINT8  = internal::DSUINT8;
  using ReportId = internal::ReportId;

  ReportId reportIdValue; // 0x00  0xF2
  DSUINT8 payload[15];    // 0x01..0x0f

  static constexpr ReportId reportId = ReportId::AuthStatus;
  static constexpr size_t size       = 16;
};
static_assert(sizeof(AuthenticationStatus) == 16, "0xF2 must stay 16 bytes");

// PS5 authentication sequence observed on every controller connection:
//   SET feature 0x08, 17 bytes, repeated a few times (purpose unclear)
//   SET feature 0xF0, four 64-byte challenge chunks (sequence 0,1,2,3)
//   GET feature 0xF2, three times, 16-byte status poll
// If 0xF2 never leaves f2 00 01 11 ..., the sequence repeats every ~10-15 s.
// 0xF1 (signed response) was never requested. All control transfers completed in
// under ~2 ms with exact lengths and zero USB errors; serializing control and
// interrupt traffic changed nothing, and usbmon matched the proxy byte-for-byte.
// Corruption, concurrency and transport errors are therefore ruled out; direct-
// connection timing or missing protocol state remains the explanation. This
// header models the traffic but does not claim it can authenticate.

// USB topology observed on a Linux USB host, USB VID 054C PID 0CE6:
//
//   interface 0  Audio (0x01) / AudioControl (0x01)    no endpoints
//   interface 1  Audio (0x01) / AudioStreaming (0x02)  alt 1: OUT iso 392 B,
//                                                       interval 4 (mic)
//   interface 2  Audio (0x01) / AudioStreaming (0x02)  alt 1: IN iso 196 B,
//                                                       interval 4 (speaker)
//   interface 3  HID (0x03)                            IN/OUT interrupt 64 B,
//                                                       interval 6 (gamepad)
//
// Interfaces 1 and 2 use alt 0 for zero bandwidth and alt 1 while streaming.
// The HID report descriptor is 289 bytes.
//
// FunctionFS notes from the same research (Linux-specific, not DualSense wire
// protocol): class-specific Audio descriptors 0x24/0x25 are rejected with
// EINVAL and must be stripped from a copied config descriptor; endpoint
// addresses are silently renumbered by position; epN files block until
// SET_CONFIGURATION; raw-gadget was not available on the stock kernel used
// for the capture, while FunctionFS/usb_f_fs was.

struct SerialNumber {
  char value[18];
  uint8_t length;

  constexpr SerialNumber() : value(), length(0) {}

  constexpr explicit SerialNumber(const char *text) : SerialNumber() {
    while (length < 17 && text[length]) {
      value[length] = text[length];
      ++length;
    }
    value[length] = 0;
  }

  constexpr const char *getColorName() const {
    if (length < 6) return "Unknown";
    static constexpr struct {
      char first;
      char second;
      const char *name;
    } kColors[] = {
        {'0', '0', "White"},
        {'0', '1', "Midnight Black"},
        {'0', '2', "Cosmic Red"},
        {'0', '3', "Nova Pink"},
        {'0', '4', "Galactic Purple"},
        {'0', '5', "Starlight Blue"},
        {'0', '6', "Grey Camouflage"},
        {'0', '7', "Volcanic Red"},
        {'0', '8', "Sterling Silver"},
        {'0', '9', "Cobalt Blue"},
        {'1', '0', "Chroma Teal"},
        {'1', '1', "Chroma Indigo"},
        {'1', '2', "Chroma Pearl"},
        {'1', '3', "HyperPop Techno Red"},
        {'1', '4', "HyperPop Remix Green"},
        {'1', '5', "HyperPop Rhythm Blue"},
        {'3', '0', "30th Anniversary"},
        {'Z', '1', "God of War Ragnarok"},
        {'Z', '2', "Spider-Man 2"},
        {'Z', '3', "Astro Bot"},
        {'Z', '4', "Fortnite"},
        {'Z', '6', "The Last of Us"},
        {'Z', 'A', "God of War 20th Anniversary"},
        {'Z', 'B', "Icon Blue Limited Edition"},
        {'Z', 'C', "Ghost of Yotei Limited Edition"},
        {'Z', 'D', "Marathon Limited Edition"},
        {'Z', 'E', "Genshin Impact Limited Edition"},
        {'Z', 'F', "007 First Light Limited Edition"},
    };
    for (auto &color : kColors)
      if (value[4] == color.first && value[5] == color.second) return color.name;
    return "Unknown";
  }
};

// Command transport shared by the 0x80 command and 0x81 response reports.
// A caller sends CommandRequest through feature report 0x80 and then reads
// CommandResponse from feature report 0x81 until Complete/CompleteMore.
enum class CommandStatus : uint8_t {
  Idle         = 0,
  Running      = 1,
  Complete     = 2,
  CompleteMore = 3,
  Timeout      = 0xFF,
};

struct CommandRequest {
private:
  using ReportId = internal::ReportId;

protected:
  ReportId reportIdValue; // 0x80
  DSUINT8 device;
  DSUINT8 action;
  DSUINT8 reserved[61];

  static constexpr ReportId reportId = ReportId::CommandHost;
  static constexpr size_t size       = 64;

public:
  constexpr CommandRequest() : reportIdValue(reportId), device(0), action(0), reserved() {}

  constexpr CommandRequest(CommandDevice deviceValue, uint8_t actionValue)
      : reportIdValue(reportId), device(static_cast<uint8_t>(deviceValue)), action(actionValue),
        reserved() {}
};
static_assert(sizeof(CommandRequest) == 64, "0x80 command must stay 64 bytes");

struct CommandResponse {
private:
  using ReportId = internal::ReportId;

public:
  ReportId reportIdValue; // 0x81
  DSUINT8 device;
  DSUINT8 action;
  DSUINT8 status;
  DSUINT8 payload[56];
  DSUINT8 reserved[4];

  static constexpr ReportId reportId = ReportId::CommandStatus;
  static constexpr size_t size       = 64;

  constexpr CommandResponse()
      : reportIdValue(reportId), device(0), action(0), status(0), payload(), reserved() {}

  constexpr bool isMatch(CommandDevice deviceValue, uint8_t actionValue) const {
    return this->reportIdValue == reportId &&
           static_cast<uint8_t>(this->device) == static_cast<uint8_t>(deviceValue) &&
           static_cast<uint8_t>(this->action) == actionValue;
  }

  constexpr bool isComplete() const {
    return static_cast<uint8_t>(this->status) == static_cast<uint8_t>(CommandStatus::Complete);
  }

  constexpr bool isCompleteMore() const {
    return static_cast<uint8_t>(this->status) == static_cast<uint8_t>(CommandStatus::CompleteMore);
  }
};
static_assert(sizeof(CommandResponse) == 64, "0x81 response must stay 64 bytes");

struct Finetune {
  uint16_t values[12];
  bool valid;
};

inline constexpr Finetune parseFinetune(const CommandResponse &response) {
  Finetune result{};
  if (!response.isComplete()) return result;
  for (unsigned i = 0; i < 12; ++i)
    result.values[i] = static_cast<uint16_t>(
        static_cast<uint8_t>(response.payload[i * 2]) |
        (static_cast<uint16_t>(static_cast<uint8_t>(response.payload[i * 2 + 1])) << 8));
  result.valid = true;
  return result;
}

struct McuUniqueId {
  uint64_t value;
  bool valid;
};

// The USB command returns one status/success byte followed by the eight-byte
// little-endian MCU ID.
inline constexpr McuUniqueId parseMcuUniqueId(const CommandResponse &response) {
  if (!response.isComplete() || static_cast<uint8_t>(response.payload[0]) != 0) return {0, false};
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= static_cast<uint64_t>(static_cast<uint8_t>(response.payload[i + 1])) << (8 * i);
  return {value, true};
}

struct BluetoothAddress {
  uint8_t bytes[6];
  bool valid;
};

// Six bytes in little-endian controller order: bytes[0] is the last MAC octet.
inline constexpr BluetoothAddress parseBluetoothAddress(const CommandResponse &response) {
  if (!response.isComplete()) return {{0, 0, 0, 0, 0, 0}, false};
  BluetoothAddress address{};
  for (unsigned i = 0; i < 6; ++i) address.bytes[i] = static_cast<uint8_t>(response.payload[i]);
  address.valid = true;
  return address;
}

struct PatchInfo {
  uint32_t id;
  uint32_t count;
  uint32_t size;
  bool valid;
};

// Read Patch Info returns three little-endian 32-bit fields; their exact
// semantics are not documented by the reference material.
inline constexpr PatchInfo parsePatchInfo(const CommandResponse &response) {
  if (!response.isComplete()) return {0, 0, 0, false};
  auto word = [&](unsigned offset) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
      value |= static_cast<uint32_t>(static_cast<uint8_t>(response.payload[offset + i])) << (8 * i);
    return value;
  };
  return {word(0), word(4), word(8), true};
}

struct DeviceInfo {
  uint8_t bytes[12];
  bool valid;
};

inline constexpr DeviceInfo parseDeviceInfo(const CommandResponse &response) {
  DeviceInfo info{};
  if (!response.isComplete()) return info;
  for (unsigned i = 0; i < 12; ++i) info.bytes[i] = static_cast<uint8_t>(response.payload[i]);
  info.valid = true;
  return info;
}

struct SpiderStatus {
  uint8_t value;
  bool valid;
};

inline constexpr SpiderStatus parseSpiderStatus(const CommandResponse &response) {
  if (!response.isComplete()) return {0, false};
  return {static_cast<uint8_t>(response.payload[0]), true};
}

// The USB HID descriptor exposes command traffic through 0x80/0x81 only.
// 0x90/0x91/0xA4 are absent, and 0xA0 is a one-byte feature report. The table,
// touch-panel and gyro/codec query reports therefore require a different
// transport or control path; they are not represented as typed results here.

} // namespace internal
} // namespace dualsense
