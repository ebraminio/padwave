#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <opus/opus.h>

namespace dualsense {
namespace internal {

template <typename Type, typename UnsignedType = Type>
struct NumType {
  static constexpr unsigned int static_size = sizeof(Type);

  constexpr NumType() = default;

  constexpr explicit NumType(Type value) { *this = value; }

  constexpr NumType &operator=(Type value) {
    UnsignedType v = static_cast<UnsignedType>(value);
    for (unsigned int i = 0; i < sizeof(Type); ++i) {
      bytes[i] = static_cast<unsigned char>((v & 0xFF));
      v = static_cast<UnsignedType>((v >> 8));
    }
    return *this;
  }

  constexpr operator Type() const {
    UnsignedType v = 0;
    for (unsigned int i = 0; i < sizeof(Type); ++i)
      v = static_cast<UnsignedType>(((v << 8) | bytes[sizeof(Type) - 1 - i]));
    return static_cast<Type>(v);
  }

  constexpr bool operator==(const NumType &o) const {
    return static_cast<Type>(*this) == static_cast<Type>(o);
  }

  constexpr bool operator!=(const NumType &o) const { return !(*this == o); }

  constexpr NumType &operator+=(Type n) { return *this = static_cast<Type>(*this) + n; }

  constexpr NumType &operator-=(Type n) { return *this = static_cast<Type>(*this) - n; }

  constexpr NumType &operator|=(Type n) { return *this = static_cast<Type>(*this) | n; }

  constexpr NumType &operator&=(Type n) { return *this = static_cast<Type>(*this) & n; }

  constexpr NumType &operator^=(Type n) { return *this = static_cast<Type>(*this) ^ n; }

  constexpr NumType &operator++() { return *this = static_cast<Type>(*this) + 1; }

  constexpr NumType &operator--() { return *this = static_cast<Type>(*this) - 1; }

  unsigned char bytes[sizeof(Type)];
};

using DSUINT8 = NumType<uint8_t>;
using DSUINT16 = NumType<uint16_t>;
using DSUINT32 = NumType<uint32_t>;
using DSINT8 = NumType<int8_t, uint8_t>;
using DSINT16 = NumType<int16_t, uint16_t>;
using DSINT32 = NumType<int32_t, uint32_t>;

static_assert(sizeof(DSUINT8) == 1, "DSUINT8 is one byte");
static_assert(sizeof(DSUINT16) == 2, "DSUINT16 is two bytes");
static_assert(sizeof(DSUINT32) == 4, "DSUINT32 is four bytes");
static_assert(sizeof(DSINT8) == 1, "DSINT8 is one byte");
static_assert(sizeof(DSINT16) == 2, "DSINT16 is two bytes");
static_assert(sizeof(DSINT32) == 4, "DSINT32 is four bytes");

// The CRC the controller checks: standard CRC-32 (IEEE 802.3, reflected
// polynomial 0xEDB88320), seeded with the stream's seed byte (0xA1 input,
// 0xA2 output, 0xA3 features), over everything before the trailing `crc`
// member. Every report type carries `crc`, `CrcSeed` and a `report_id`, so one
// template covers them all. This is the same result as zlib's crc32(), but
// local so the binary does not need -lz.
struct Crc32Table {
  uint32_t entries[256];
};

static constexpr Crc32Table makeCrc32Table() {
  Crc32Table table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (unsigned bit = 0; bit < 8; ++bit) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    table.entries[i] = c;
  }
  return table;
}

static constexpr Crc32Table kCrc32Table = makeCrc32Table();

static constexpr uint32_t crc32(uint32_t crc, const uint8_t *p, size_t len) {
  crc = ~crc;
  while (len--) crc = kCrc32Table.entries[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

template <typename Report>
struct WireBytes {
  unsigned char data[sizeof(Report)];
};

template <typename Report>
static constexpr uint32_t crc_of(const Report &report) {
  uint8_t seed = static_cast<uint8_t>(Report::CrcSeed);
  uint32_t c = crc32(0, &seed, 1);
  auto raw = __builtin_bit_cast(WireBytes<Report>, report);
  return crc32(c, raw.data, sizeof(Report) - sizeof(DSUINT32));
}

// ===========================================================================
// Output: the 47-byte state body shared by 0x31 (Bluetooth) and 0x02 (USB)
//
// +0x00 in these comments is the offset inside this payload: 0x01 in a USB
// report, 0x03 in a Bluetooth one (after the id, the sequence tag and the 0x10
// tag byte). Only the fields whose valid_flag* bit is set are applied by the
// controller — a claim, not a change notification — which is what makes a
// lightbar-only update safe while the audio stream is running.
// ===========================================================================

struct OutputBodyProbe;
struct AudioReportProbe;
} // namespace internal

// Report ordinals, common to every schema. 0x31 is both the input and the
// output state report; the direction is the stream, not the id.

enum class Packet : uint8_t {
  PacketInputUsb = 0x01,
  PacketOutputUsb = 0x02,
  PacketFeatureCalibration = 0x05,
  PacketFeaturePairing = 0x09,
  PacketFeatureFirmware = 0x20,
  PacketStateBluetooth = 0x31,
  PacketAudio = 0x36,
};

struct OutputBody {
  friend struct internal::OutputBodyProbe;

public:
  // Canonical 11-byte adaptive-trigger block: type byte followed by ten
  // parameters. Encoders adapted from ExtendInput.DataTools.DualSense
  // TriggerEffectGenerator (revision 6) by John "Nielk1" Klein, MIT License.
  struct TriggerEffect {
  public:
    // All effect byte values from TriggerEffectType. The Simple/Limited/Debug
    // values are firmware leftovers; keep them for completeness but prefer the
    // official factories.
    enum class Type : uint8_t {
      Off = 0x05,
      Feedback = 0x21,
      Weapon = 0x25,
      Vibration = 0x26,
      Bow = 0x22,
      Galloping = 0x23,
      Machine = 0x27,
      SimpleFeedback = 0x01,
      SimpleWeapon = 0x02,
      SimpleVibration = 0x06,
      LimitedFeedback = 0x11,
      LimitedWeapon = 0x12,
      DebugFc = 0xFC,
      DebugFd = 0xFD,
      DebugFe = 0xFE,
    };

    static constexpr size_t BlockSize = 11;
    static constexpr int ZoneCount = 10;
    static constexpr uint8_t MaxLevel = 8;

  private:
    friend struct internal::OutputBodyProbe;
    friend struct OutputBody;

    uint8_t block[BlockSize] = {static_cast<uint8_t>(Type::Off)};

  public:
    constexpr TriggerEffect() { off(); }

    constexpr uint8_t mode() const { return block[0]; }

    constexpr uint8_t parameter(size_t index) const { return block[index + 1]; }

    constexpr void zero() {
      for (size_t i = 1; i < BlockSize; i++) block[i] = 0;
    }

    constexpr bool off() {
      block[0] = static_cast<uint8_t>(Type::Off);
      zero();
      return true;
    }

    // Official: constant resistance from `position` to the end.
    constexpr bool feedback(uint8_t position, uint8_t strength) {
      if (position > 9 || strength > 8) return false;
      if (strength == 0) return off();
      zero();
      block[0] = static_cast<uint8_t>(Type::Feedback);
      uint16_t active = 0;
      uint32_t force = 0;
      uint8_t value = static_cast<uint8_t>((strength - 1) & 0x07);
      for (int i = position; i < 10; i++) {
        active = static_cast<uint16_t>(active | (uint16_t{1} << i));
        force |= static_cast<uint32_t>(value) << (3 * i);
      }
      block[1] = static_cast<uint8_t>(active & 0xFF);
      block[2] = static_cast<uint8_t>(active >> 8);
      block[3] = static_cast<uint8_t>(force & 0xFF);
      block[4] = static_cast<uint8_t>((force >> 8) & 0xFF);
      block[5] = static_cast<uint8_t>((force >> 16) & 0xFF);
      block[6] = static_cast<uint8_t>((force >> 24) & 0xFF);
      return true;
    }

    // Official: resistance between start and end, releasing past the end.
    constexpr bool weapon(uint8_t start, uint8_t end, uint8_t strength) {
      if (start > 7 || start < 2 || end > 8 || end <= start || strength > 8) return false;
      if (strength == 0) return off();
      zero();
      uint16_t zones = static_cast<uint16_t>((uint16_t{1} << start) | (uint16_t{1} << end));
      block[0] = static_cast<uint8_t>(Type::Weapon);
      block[1] = static_cast<uint8_t>(zones & 0xFF);
      block[2] = static_cast<uint8_t>(zones >> 8);
      block[3] = static_cast<uint8_t>(strength - 1);
      return true;
    }

    // Official: vibration from `position` onward.
    constexpr bool vibration(uint8_t position, uint8_t amplitude, uint8_t frequency) {
      if (position > 9 || amplitude > 8) return false;
      if (amplitude == 0 || frequency == 0) return off();
      zero();
      block[0] = static_cast<uint8_t>(Type::Vibration);
      uint16_t active = 0;
      uint32_t levels = 0;
      uint8_t value = static_cast<uint8_t>((amplitude - 1) & 0x07);
      for (int i = position; i < 10; i++) {
        active = static_cast<uint16_t>(active | (uint16_t{1} << i));
        levels |= static_cast<uint32_t>(value) << (3 * i);
      }
      block[1] = static_cast<uint8_t>(active & 0xFF);
      block[2] = static_cast<uint8_t>(active >> 8);
      block[3] = static_cast<uint8_t>(levels & 0xFF);
      block[4] = static_cast<uint8_t>((levels >> 8) & 0xFF);
      block[5] = static_cast<uint8_t>((levels >> 16) & 0xFF);
      block[6] = static_cast<uint8_t>((levels >> 24) & 0xFF);
      block[9] = frequency;
      return true;
    }

    // Official: per-zone resistance levels (0..8).
    constexpr bool multiplePositionFeedback(const uint8_t *strength) {
      bool any = false;
      for (int i = 0; i < 10; i++)
        if (strength[i] > 0) any = true;
      if (!any) return off();
      zero();
      block[0] = static_cast<uint8_t>(Type::Feedback);
      uint16_t active = 0;
      uint32_t force = 0;
      for (int i = 0; i < 10; i++) {
        if (strength[i] == 0) continue;
        uint8_t value = static_cast<uint8_t>((strength[i] - 1) & 0x07);
        active = static_cast<uint16_t>(active | (uint16_t{1} << i));
        force |= static_cast<uint32_t>(value) << (3 * i);
      }
      block[1] = static_cast<uint8_t>(active & 0xFF);
      block[2] = static_cast<uint8_t>(active >> 8);
      block[3] = static_cast<uint8_t>(force & 0xFF);
      block[4] = static_cast<uint8_t>((force >> 8) & 0xFF);
      block[5] = static_cast<uint8_t>((force >> 16) & 0xFF);
      block[6] = static_cast<uint8_t>((force >> 24) & 0xFF);
      return true;
    }

    // Official: linearly changing resistance between two positions.
    constexpr bool slopeFeedback(uint8_t start, uint8_t end, uint8_t start_strength,
                                 uint8_t end_strength) {
      if (start > 8 || end > 9 || end <= start) return false;
      if (start_strength < 1 || start_strength > 8 || end_strength < 1 || end_strength > 8)
        return false;
      uint8_t strength[10] = {};
      float slope = static_cast<float>(static_cast<int>(end_strength) - start_strength) /
                    static_cast<float>(end - start);
      for (int i = start; i < 10; i++) {
        int value = i <= end ? static_cast<int>(static_cast<float>(start_strength) +
                                                slope * static_cast<float>(i - start) + 0.5f)
                             : end_strength;
        if (value < 0) value = 0;
        if (value > 8) value = 8;
        strength[i] = static_cast<uint8_t>(value);
      }
      return multiplePositionFeedback(strength);
    }

    // Official: per-zone vibration amplitudes with one frequency.
    constexpr bool multiplePositionVibration(uint8_t frequency, const uint8_t *amplitude) {
      bool any = false;
      for (int i = 0; i < 10; i++)
        if (amplitude[i] > 0) any = true;
      if (frequency == 0 || !any) return off();
      zero();
      block[0] = static_cast<uint8_t>(Type::Vibration);
      uint16_t active = 0;
      uint32_t levels = 0;
      for (int i = 0; i < 10; i++) {
        if (amplitude[i] == 0) continue;
        uint8_t value = static_cast<uint8_t>((amplitude[i] - 1) & 0x07);
        active = static_cast<uint16_t>(active | (uint16_t{1} << i));
        levels |= static_cast<uint32_t>(value) << (3 * i);
      }
      block[1] = static_cast<uint8_t>(active & 0xFF);
      block[2] = static_cast<uint8_t>(active >> 8);
      block[3] = static_cast<uint8_t>(levels & 0xFF);
      block[4] = static_cast<uint8_t>((levels >> 8) & 0xFF);
      block[5] = static_cast<uint8_t>((levels >> 16) & 0xFF);
      block[6] = static_cast<uint8_t>((levels >> 24) & 0xFF);
      block[9] = frequency;
      return true;
    }

    // Unofficial firmware effects.
    constexpr bool bow(uint8_t start, uint8_t end, uint8_t strength, uint8_t snap) {
      if (start > 8 || end > 8 || start >= end || strength > 8 || snap > 8) return false;
      if (end == 0 || strength == 0 || snap == 0) return off();
      zero();
      uint16_t zones = static_cast<uint16_t>((uint16_t{1} << start) | (uint16_t{1} << end));
      uint32_t pair = static_cast<uint32_t>((strength - 1) & 0x07) |
                      (static_cast<uint32_t>((snap - 1) & 0x07) << 3);
      block[0] = static_cast<uint8_t>(Type::Bow);
      block[1] = static_cast<uint8_t>(zones & 0xFF);
      block[2] = static_cast<uint8_t>(zones >> 8);
      block[3] = static_cast<uint8_t>(pair & 0xFF);
      block[4] = static_cast<uint8_t>((pair >> 8) & 0xFF);
      return true;
    }

    constexpr bool galloping(uint8_t start, uint8_t end, uint8_t first_foot, uint8_t second_foot,
                             uint8_t frequency) {
      if (start > 8 || end > 9 || start >= end || second_foot > 7 || first_foot > 6 ||
          first_foot >= second_foot)
        return false;
      if (frequency == 0) return off();
      zero();
      uint16_t zones = static_cast<uint16_t>((uint16_t{1} << start) | (uint16_t{1} << end));
      uint32_t ratio = static_cast<uint32_t>(second_foot & 0x07) |
                       (static_cast<uint32_t>(first_foot & 0x07) << 3);
      block[0] = static_cast<uint8_t>(Type::Galloping);
      block[1] = static_cast<uint8_t>(zones & 0xFF);
      block[2] = static_cast<uint8_t>(zones >> 8);
      block[3] = static_cast<uint8_t>(ratio & 0xFF);
      block[4] = frequency;
      return true;
    }

    constexpr bool machine(uint8_t start, uint8_t end, uint8_t amplitude_a, uint8_t amplitude_b,
                           uint8_t frequency, uint8_t period) {
      if (start > 8 || end > 9 || end <= start || amplitude_a > 7 || amplitude_b > 7) return false;
      if (frequency == 0) return off();
      zero();
      uint16_t zones = static_cast<uint16_t>((uint16_t{1} << start) | (uint16_t{1} << end));
      uint8_t pair = static_cast<uint8_t>((amplitude_a & 0x07) | ((amplitude_b & 0x07) << 3));
      block[0] = static_cast<uint8_t>(Type::Machine);
      block[1] = static_cast<uint8_t>(zones & 0xFF);
      block[2] = static_cast<uint8_t>(zones >> 8);
      block[3] = pair;
      block[4] = frequency;
      block[5] = period;
      return true;
    }

    // Firmware leftovers with simpler logic and no parameter validation.
    constexpr bool simpleFeedback(uint8_t position, uint8_t strength) {
      zero();
      block[0] = static_cast<uint8_t>(Type::SimpleFeedback);
      block[1] = position;
      block[2] = strength;
      return true;
    }

    constexpr bool simpleWeapon(uint8_t start, uint8_t end, uint8_t strength) {
      zero();
      block[0] = static_cast<uint8_t>(Type::SimpleWeapon);
      block[1] = start;
      block[2] = end;
      block[3] = strength;
      return true;
    }

    constexpr bool simpleVibration(uint8_t position, uint8_t amplitude, uint8_t frequency) {
      if (frequency == 0 || amplitude == 0) return off();
      zero();
      block[0] = static_cast<uint8_t>(Type::SimpleVibration);
      block[1] = frequency;
      block[2] = amplitude;
      block[3] = position;
      return true;
    }

    // Firmware leftovers with narrower validation ranges.
    constexpr bool limitedFeedback(uint8_t position, uint8_t strength) {
      if (strength > 10) return false;
      if (strength == 0) return off();
      zero();
      block[0] = static_cast<uint8_t>(Type::LimitedFeedback);
      block[1] = position;
      block[2] = strength;
      return true;
    }

    constexpr bool limitedWeapon(uint8_t start, uint8_t end, uint8_t strength) {
      if (start < 0x10 || end < start || static_cast<int>(end) > static_cast<int>(start) + 100 ||
          strength > 10)
        return false;
      if (strength == 0) return off();
      zero();
      block[0] = static_cast<uint8_t>(Type::LimitedWeapon);
      block[1] = start;
      block[2] = end;
      block[3] = strength;
      return true;
    }

    // Apple-style normalized [0,1] adapters from the same generator.
    static constexpr uint8_t normalize(float value, float scale) {
      if (!(value > -1.0e30f && value < 1.0e30f)) return 0;
      if (value < 0) value = 0;
      if (value > 1) value = 1;
      int scaled = static_cast<int>(value * scale + 0.5f);
      return static_cast<uint8_t>(scaled);
    }

    constexpr bool appleFeedbackWithStartPosition(float start, float strength) {
      return feedback(normalize(start, 9.0f), normalize(strength, 8.0f));
    }

    constexpr bool appleWeaponWithStartPosition(float start, float end, float strength) {
      return weapon(normalize(start, 9.0f), normalize(end, 9.0f), normalize(strength, 8.0f));
    }

    constexpr bool appleVibrationWithStartPosition(float start, float amplitude, float frequency) {
      return vibration(normalize(start, 9.0f), normalize(amplitude, 8.0f),
                       normalize(frequency, 255.0f));
    }

    constexpr bool appleFeedback(const float *strengths) {
      uint8_t levels[10];
      for (int i = 0; i < 10; i++) levels[i] = normalize(strengths[i], 8.0f);
      return multiplePositionFeedback(levels);
    }

    constexpr bool appleSlopeFeedback(float start, float end, float start_strength,
                                      float end_strength) {
      return slopeFeedback(normalize(start, 9.0f), normalize(end, 9.0f),
                           normalize(start_strength, 8.0f), normalize(end_strength, 8.0f));
    }

    constexpr bool appleVibration(const float *amplitudes, float frequency) {
      uint8_t levels[10];
      for (int i = 0; i < 10; i++) levels[i] = normalize(amplitudes[i], 8.0f);
      return multiplePositionVibration(normalize(frequency, 255.0f), levels);
    }

    // reWASD traffic captures, including its intentionally non-standard values.
    constexpr bool rewasdFullPress() { return simpleWeapon(0x90, 0xA0, 0xFF); }

    constexpr bool rewasdSoftPress() { return simpleWeapon(0x70, 0xA0, 0xFF); }

    constexpr bool rewasdMediumPress() { return simpleWeapon(0x45, 0xA0, 0xFF); }

    constexpr bool rewasdHardPress() { return simpleWeapon(0x20, 0xA0, 0xFF); }

    constexpr bool rewasdPulse() { return simpleWeapon(0x00, 0x00, 0x00); }

    constexpr bool rewasdChoppy() {
      zero();
      block[0] = static_cast<uint8_t>(Type::Feedback);
      block[1] = 0x02;
      block[2] = 0x27;
      block[3] = 0x18;
      block[4] = 0x00;
      block[5] = 0x00;
      block[6] = 0x26;
      return true;
    }

    constexpr bool rewasdSoftRigidity() { return simpleFeedback(0x00, 0x00); }

    constexpr bool rewasdMediumRigidity() { return simpleFeedback(0x00, 0x64); }

    constexpr bool rewasdMaxRigidity() { return simpleFeedback(0x00, 0xDC); }

    constexpr bool rewasdHalfPress() { return simpleFeedback(0x55, 0x64); }

    constexpr bool rewasdRifle(uint8_t frequency = 10) {
      if (frequency < 2 || frequency > 20) return false;
      zero();
      block[0] = static_cast<uint8_t>(Type::Vibration);
      block[2] = 0x03;
      block[6] = 0x3F;
      block[9] = frequency;
      return true;
    }

    constexpr bool rewasdVibration(uint8_t strength = 220, uint8_t frequency = 30) {
      if (strength < 1 || frequency < 1) return false;
      zero();
      block[0] = static_cast<uint8_t>(Type::Vibration);
      block[2] = 0x03;
      block[6] = strength;
      block[9] = frequency;
      return true;
    }
  };

private:
  using DSUINT8 = internal::DSUINT8;
  using DSUINT32 = internal::DSUINT32;

protected:
  DSUINT8 valid_flag0;         // +0x00  ValidFlag0 bits
  DSUINT8 valid_flag1;         // +0x01  ValidFlag1 bits
  DSUINT8 motor_right;         // +0x02  DS4-compatible rumble
  DSUINT8 motor_left;          // +0x03
  DSUINT8 headphone_volume;    // +0x04  0x00..0x7f
  DSUINT8 speaker_volume;      // +0x05  0x00..0xff
  DSUINT8 mic_volume;          // +0x06  0x00..0x40 (claimed by MicVolume)
  DSUINT8 audio_control;       // +0x07  AudioControl; claimed by AudioRouting
  DSUINT8 mute_button_led;     // +0x08
  DSUINT8 power_save_control;  // +0x09  PowerSave bits
  TriggerEffect right_trigger; // +0x0a  11-byte effect block
  TriggerEffect left_trigger;  // +0x15  11-byte effect block
  DSUINT32 host_timestamp;     // +0x20  mirrored input sensor timestamp
  DSUINT8 motor_power_level;   // +0x24  trigger:rumble reduction nibbles
  DSUINT8 audio_control2;      // +0x25  speaker preamp gain
  DSUINT8 valid_flag2;         // +0x26  ValidFlag2 bits
  DSUINT8 haptic_low_pass_filter; // +0x27  hardware haptic LPF enable
  DSUINT8 reserved_40;            // +0x28
  DSUINT8 lightbar_setup;      // +0x29  valid_flag2 bit 1; 0x02 fades out
  DSUINT8 led_brightness;      // +0x2a
  DSUINT8 player_leds;         // +0x2b  PlayerLeds bits
  DSUINT8 lightbar_red;        // +0x2c
  DSUINT8 lightbar_green;      // +0x2d
  DSUINT8 lightbar_blue;       // +0x2e

public:
  enum : uint8_t { Size = 47 };

  constexpr uint8_t headphoneVolume() const { return headphone_volume; }

  constexpr uint8_t speakerVolume() const { return speaker_volume; }

private:
  // Which fields this report owns: the kernel's (and DS4Windows', and
  // dualsense-tester's) valid_flag0 / valid_flag1 / valid_flag2. A set bit
  // means "adopt my bytes for this feature"; a clear bit means "leave that
  // feature as it is" — a claim, not a change notification.
  //
  // Example: RightTriggerMotorEnable | LeftTriggerMotorEnable claims only the two
  // trigger blocks,
  // leaving volumes, LEDs and rumble untouched.
  enum class ValidFlag0 : uint8_t {
    CompatibleVibration = 1u << 0,     // +0x00 bit 0; main motors also need HapticsSelect
    HapticsSelect = 1u << 1,           //       bit 1; paired with CompatibleVibration
    RightTriggerMotorEnable = 1u << 2, //       bit 2
    LeftTriggerMotorEnable = 1u << 3,  //       bit 3
    HeadphoneVolumeEnable = 1u << 4,   //       bit 4
    SpeakerVolumeEnable = 1u << 5,     //       bit 5
    MicVolumeEnable = 1u << 6,         //       bit 6
    AudioControlEnable = 1u << 7,      //       bit 7
  };

  enum class ValidFlag1 : uint8_t {
    MicMuteLedControlEnable = 1u << 0,      // +0x01 bit 0
    PowerSaveControlEnable = 1u << 1,       //       bit 1  enables power_save_control
    LightbarControlEnable = 1u << 2,        //       bit 2
    ReleaseLEDs = 1u << 3,                  //       bit 3
    PlayerIndicatorControlEnable = 1u << 4, //       bit 4
    HapticLowPassFilterEnable = 1u << 5,    //       bit 5
    VibrationAttenuationEnable = 1u << 6,   //       bit 6
    AudioControl2Enable = 1u << 7,          //       bit 7
  };

  // +0x26. Only two bits are documented anywhere in the references: bit 0
  // changes the LED brightness and bit 1 enables the fade-out control. Bits
  // 3-7 are reserved.
  enum class ValidFlag2 : uint8_t {
    LedBrightnessControlEnable = 1u << 0, // bit 0
    LightbarSetupControlEnable = 1u << 1, // bit 1: enables the lightbar_setup fade control
    CompatibleVibration2 = 1u << 2,       // bit 2
  };

  // +0x09. Bit layout: bits 0-3 reserved, bit 4 microphone mute, bit 5
  // reserved, bit 6 audio mute, bit 7 reserved. Only the two documented bits
  // are named (hid-playstation.c uses the first; DS4Windows comments both);
  // the other six are in the panel's power save row so they can be probed: if
  // one stops the motion sensors, the gyro and accelerometer in the UI go flat.
  enum PowerSave : uint8_t {
    TouchPowerSave = 1u << 0,     // bit 0
    MotionPowerSave = 1u << 1,    // bit 1
    HapticPowerSave = 1u << 2,    // bit 2
    AudioPowerSave = 1u << 3,     // bit 3
    MicMute = 1u << 4,            // bit 4
    SpeakerMute = 1u << 5,        // bit 5
    HeadphoneMute = 1u << 6,      // bit 6
    HapticMute = 1u << 7,         // bit 7
  };

  // +0x2b: the five player LEDs, the same pattern the charging sweep animates.
  // Bits 5-7 are reserved.
  enum class PlayerLeds : uint8_t {
    Led1 = 1u << 0,    // bit 0
    Led2 = 1u << 1,    // bit 1
    Led3 = 1u << 2,    // bit 2
    Led4 = 1u << 3,    // bit 3
    Led5 = 1u << 4,    // bit 4
    Instant = 1u << 5, // bit 5: suppress the slow fade-in
  };

public:
  enum class LightbarSetup : uint8_t {
    LightOn = 1u << 0,
    LightOut = 1u << 1,
  };

  // +0x07: the output path. 0x09 is the membrane speaker, 0x00 the headphone
  // jack; the audio service patched the snapshot to 0x00 when the jack is in
  // use, which is why a silent jack was the first symptom of getting this
  // wrong.
  enum class AudioOutputPath : uint8_t {
    L_R_X = 0,
    L_L_X = 1,
    L_L_R = 2,
    X_X_R = 3,
  };

  enum class AudioControl : uint8_t {
    Headphones = 0x00,
    Speaker = static_cast<uint8_t>(AudioOutputPath::X_X_R) << 4,
  };

private:
  static constexpr uint8_t bit(ValidFlag0 f) { return static_cast<uint8_t>(f); }

  static constexpr uint8_t bit(ValidFlag1 f) { return static_cast<uint8_t>(f); }

  static constexpr uint8_t bit(ValidFlag2 f) { return static_cast<uint8_t>(f); }

  constexpr void claim(ValidFlag0 f) { valid_flag0 = static_cast<uint8_t>(valid_flag0 | bit(f)); }

  constexpr void claim(ValidFlag1 f) { valid_flag1 = static_cast<uint8_t>(valid_flag1 | bit(f)); }

  constexpr void claim(ValidFlag2 f) { valid_flag2 = static_cast<uint8_t>(valid_flag2 | bit(f)); }

public:
  constexpr void clearOneShotLightbar() {
    valid_flag1 = static_cast<uint8_t>(valid_flag1 & ~bit(ValidFlag1::ReleaseLEDs));
    valid_flag2 = static_cast<uint8_t>(valid_flag2 & ~bit(ValidFlag2::LightbarSetupControlEnable));
  }

  constexpr void setMotorRight(uint8_t v) {
    claim(ValidFlag0::CompatibleVibration);
    claim(ValidFlag0::HapticsSelect);
    motor_right = v;
  }

  constexpr void setMotorLeft(uint8_t v) {
    claim(ValidFlag0::CompatibleVibration);
    claim(ValidFlag0::HapticsSelect);
    motor_left = v;
  }

  constexpr void setHeadphoneVolume(uint8_t v) {
    claim(ValidFlag0::HeadphoneVolumeEnable);
    headphone_volume = v;
  }

  constexpr void setSpeakerVolume(uint8_t v) {
    claim(ValidFlag0::SpeakerVolumeEnable);
    speaker_volume = v;
  }

  constexpr void setMicVolume(uint8_t v) {
    claim(ValidFlag0::MicVolumeEnable);
    mic_volume = v;
  }

  constexpr void setAudioRouting(uint8_t v) {
    claim(ValidFlag0::AudioControlEnable);
    audio_control = v;
  }

  constexpr void setMuteButtonLed(uint8_t v) {
    claim(ValidFlag1::MicMuteLedControlEnable);
    mute_button_led = v;
  }

  constexpr void setPowerSaveControl(uint8_t v) {
    claim(ValidFlag1::PowerSaveControlEnable);
    power_save_control = v;
  }

  constexpr void setRightTriggerMode(uint8_t v) {
    claim(ValidFlag0::RightTriggerMotorEnable);
    right_trigger.block[0] = v;
  }

  constexpr void setLeftTriggerMode(uint8_t v) {
    claim(ValidFlag0::LeftTriggerMotorEnable);
    left_trigger.block[0] = v;
  }

  constexpr void setMotorPowerLevel(uint8_t v) {
    claim(ValidFlag1::VibrationAttenuationEnable);
    motor_power_level = v;
  }

  constexpr void setHapticLowPassFilter(bool on) {
    claim(ValidFlag1::HapticLowPassFilterEnable);
    haptic_low_pass_filter = on ? 1 : 0;
  }

  constexpr void setHapticLowPassFilter(uint8_t v) { setHapticLowPassFilter(v != 0); }

  constexpr void setHostTimestamp(uint32_t v) { host_timestamp = v; }

  constexpr void setAudioControl2(uint8_t v) {
    claim(ValidFlag1::AudioControl2Enable);
    audio_control2 = v;
  }

  constexpr void setLightbarSetup(LightbarSetup v) {
    claim(ValidFlag2::LightbarSetupControlEnable);
    lightbar_setup = static_cast<uint8_t>(v);
  }

  constexpr void setLedBrightness(uint8_t v) {
    claim(ValidFlag2::LedBrightnessControlEnable);
    led_brightness = v;
  }

  constexpr void setPlayerLedsMask(uint8_t v) {
    claim(ValidFlag1::PlayerIndicatorControlEnable);
    player_leds = v;
  }

  constexpr void setLightbarRed(uint8_t v) {
    claim(ValidFlag1::LightbarControlEnable);
    lightbar_red = v;
  }

  constexpr void setLightbarGreen(uint8_t v) {
    claim(ValidFlag1::LightbarControlEnable);
    lightbar_green = v;
  }

  constexpr void setLightbarBlue(uint8_t v) {
    claim(ValidFlag1::LightbarControlEnable);
    lightbar_blue = v;
  }

  constexpr void setRightTriggerParams(const uint8_t *v) {
    claim(ValidFlag0::RightTriggerMotorEnable);
    for (size_t i = 0; i < 10; ++i) right_trigger.block[i + 1] = v[i];
  }

  constexpr void setLeftTriggerParams(const uint8_t *v) {
    claim(ValidFlag0::LeftTriggerMotorEnable);
    for (size_t i = 0; i < 10; ++i) left_trigger.block[i + 1] = v[i];
  }

  constexpr void setRightTriggerEffect(const TriggerEffect &effect) {
    claim(ValidFlag0::RightTriggerMotorEnable);
    right_trigger = effect;
  }

  constexpr void setLeftTriggerEffect(const TriggerEffect &effect) {
    claim(ValidFlag0::LeftTriggerMotorEnable);
    left_trigger = effect;
  }

  constexpr void setAudioRoute(AudioControl routing) {
    claim(ValidFlag0::AudioControlEnable);
    audio_control = static_cast<uint8_t>(routing);
  }

  constexpr void setAudio(AudioControl routing) {
    valid_flag0 = static_cast<uint8_t>(
        bit(ValidFlag0::HeadphoneVolumeEnable) | bit(ValidFlag0::SpeakerVolumeEnable) |
        bit(ValidFlag0::MicVolumeEnable) | bit(ValidFlag0::AudioControlEnable));
    valid_flag1 = static_cast<uint8_t>(valid_flag1 | bit(ValidFlag1::AudioControl2Enable));
    headphone_volume = 0x7F;
    speaker_volume = 0x64;
    audio_control = static_cast<uint8_t>(routing);
    audio_control2 = 0x0A;
  }

  constexpr void setMic(bool on) {
    valid_flag1 = static_cast<uint8_t>(valid_flag1 | bit(ValidFlag1::PowerSaveControlEnable));
    if (on) {
      power_save_control = static_cast<uint8_t>(power_save_control & ~0x10);
      mic_volume = 0x40;
    } else {
      power_save_control = static_cast<uint8_t>(power_save_control | 0x10);
    }
  }

  constexpr void clearPlayerLeds() {
    valid_flag1 =
        static_cast<uint8_t>(valid_flag1 & ~bit(ValidFlag1::PlayerIndicatorControlEnable));
  }

  constexpr void clearLightbar() {
    valid_flag1 = static_cast<uint8_t>(valid_flag1 & ~bit(ValidFlag1::LightbarControlEnable));
    valid_flag2 = static_cast<uint8_t>(valid_flag2 & ~bit(ValidFlag2::LightbarSetupControlEnable));
    lightbar_setup = 0;
    lightbar_red = 0;
    lightbar_green = 0;
    lightbar_blue = 0;
  }

  // Claims only what the audio path writes: the volumes and the routing.
  // Claiming anything else here is how one writer silently owns a feature
  // another writer is using.
  constexpr void init(AudioControl routing = AudioControl::Headphones) {
    *this = OutputBody{};
    valid_flag0 = static_cast<uint8_t>(
        bit(ValidFlag0::HeadphoneVolumeEnable) | bit(ValidFlag0::SpeakerVolumeEnable) |
        bit(ValidFlag0::MicVolumeEnable) | bit(ValidFlag0::AudioControlEnable));
    valid_flag1 = bit(ValidFlag1::AudioControl2Enable);
    headphone_volume = 0x7F;
    speaker_volume = 0x64;
    audio_control = static_cast<uint8_t>(routing);
    audio_control2 = 0x0A;
  }
};

static_assert(sizeof(OutputBody) == OutputBody::Size, "output body must stay 47 bytes");

namespace internal {
struct OutputBodyProbe {
  static constexpr uint8_t valid0(const OutputBody &b) { return b.valid_flag0; }

  static constexpr uint8_t valid1(const OutputBody &b) { return b.valid_flag1; }

  static constexpr uint8_t triggerByte(const OutputBody::TriggerEffect &e, size_t i) {
    return e.block[i];
  }
};
} // namespace internal


static_assert(sizeof(OutputBody::TriggerEffect) == OutputBody::TriggerEffect::BlockSize);

constexpr OutputBody::TriggerEffect kFeedbackEffect = [] {
  OutputBody::TriggerEffect effect;
  const uint8_t levels[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  effect.multiplePositionFeedback(levels);
  return effect;
}();
static_assert(internal::OutputBodyProbe::triggerByte(kFeedbackEffect, 0) ==
              static_cast<uint8_t>(OutputBody::TriggerEffect::Type::Feedback));
static_assert(internal::OutputBodyProbe::triggerByte(kFeedbackEffect, 1) == 0xFF && internal::OutputBodyProbe::triggerByte(kFeedbackEffect, 2) == 0x03);
static_assert(internal::OutputBodyProbe::triggerByte(kFeedbackEffect, 3) == 0 && internal::OutputBodyProbe::triggerByte(kFeedbackEffect, 6) == 0);

constexpr OutputBody::TriggerEffect kWeaponEffect = [] {
  OutputBody::TriggerEffect effect;
  effect.weapon(2, 8, 8);
  return effect;
}();
static_assert(internal::OutputBodyProbe::triggerByte(kWeaponEffect, 0) ==
              static_cast<uint8_t>(OutputBody::TriggerEffect::Type::Weapon));
static_assert(internal::OutputBodyProbe::triggerByte(kWeaponEffect, 1) == 0x04 && internal::OutputBodyProbe::triggerByte(kWeaponEffect, 2) == 0x01);
static_assert(internal::OutputBodyProbe::triggerByte(kWeaponEffect, 3) == 7);

constexpr OutputBody::TriggerEffect kVibrationEffect = [] {
  OutputBody::TriggerEffect effect;
  const uint8_t amplitudes[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  effect.multiplePositionVibration(25, amplitudes);
  return effect;
}();
static_assert(internal::OutputBodyProbe::triggerByte(kVibrationEffect, 0) ==
              static_cast<uint8_t>(OutputBody::TriggerEffect::Type::Vibration));
static_assert(internal::OutputBodyProbe::triggerByte(kVibrationEffect, 1) == 0x01 && internal::OutputBodyProbe::triggerByte(kVibrationEffect, 9) == 25);

constexpr OutputBody::TriggerEffect kBowEffect = [] {
  OutputBody::TriggerEffect effect;
  effect.bow(2, 8, 6, 4);
  return effect;
}();
static_assert(internal::OutputBodyProbe::triggerByte(kBowEffect, 0) == static_cast<uint8_t>(OutputBody::TriggerEffect::Type::Bow));
static_assert(internal::OutputBodyProbe::triggerByte(kBowEffect, 1) == 0x04 && internal::OutputBodyProbe::triggerByte(kBowEffect, 2) == 0x01);
static_assert(internal::OutputBodyProbe::triggerByte(kBowEffect, 3) == 0x1D);

constexpr OutputBody::TriggerEffect kGallopingEffect = [] {
  OutputBody::TriggerEffect effect;
  effect.galloping(2, 8, 2, 4, 25);
  return effect;
}();
static_assert(internal::OutputBodyProbe::triggerByte(kGallopingEffect, 0) ==
              static_cast<uint8_t>(OutputBody::TriggerEffect::Type::Galloping));
static_assert(internal::OutputBodyProbe::triggerByte(kGallopingEffect, 4) == 25);

constexpr OutputBody::TriggerEffect kMachineEffect = [] {
  OutputBody::TriggerEffect effect;
  effect.machine(2, 8, 6, 4, 25, 10);
  return effect;
}();
static_assert(internal::OutputBodyProbe::triggerByte(kMachineEffect, 0) ==
              static_cast<uint8_t>(OutputBody::TriggerEffect::Type::Machine));
static_assert(internal::OutputBodyProbe::triggerByte(kMachineEffect, 4) == 25 && internal::OutputBodyProbe::triggerByte(kMachineEffect, 5) == 10);

// Saturating conversions used by the Swift schema when moving normalized UI
// values onto the byte/zone/level wire fields.
struct DSScale {
  static constexpr float clamp01(float value) {
    if (!(value > -1.0e30f && value < 1.0e30f)) return 0;
    return value < 0 ? 0 : (value > 1 ? 1 : value);
  }

  static constexpr uint8_t byte(float value) {
    float v = clamp01(value);
    int scaled = static_cast<int>(v * 255.0f + 0.5f);
    return static_cast<uint8_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
  }

  static constexpr uint8_t rawByte(float value) {
    if (!(value > -1.0e30f && value < 1.0e30f)) return 0;
    int scaled = static_cast<int>(value + (value >= 0 ? 0.5f : -0.5f));
    return static_cast<uint8_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
  }

  static constexpr int zone(float value) {
    float v = clamp01(value) * 9.0f;
    int scaled = static_cast<int>(v + 0.5f);
    return scaled < 0 ? 0 : (scaled > 9 ? 9 : scaled);
  }

  static constexpr uint8_t level(float value) {
    float v = clamp01(value) * 8.0f;
    int scaled = static_cast<int>(v + 0.5f);
    return static_cast<uint8_t>(scaled < 0 ? 0 : (scaled > 8 ? 8 : scaled));
  }
};

// ===========================================================================
// Output 0x31 (Bluetooth, 78 bytes, CRC at the end)
// ===========================================================================

// Implementation-only wire wrappers. Consumers use InputReport, OutputReport,
// UsbOutputReport, AudioReport and MicFrame from padwave.
struct OutputReport {
private:
  using DSUINT8 = internal::DSUINT8;
  using DSUINT32 = internal::DSUINT32;

protected:
  DSUINT8 report_id;     // 0x00  Packet::PacketStateBluetooth
  DSUINT8 seq_tag;       // 0x01  high nibble = counter 0..15, low nibble = 0
  DSUINT8 tag;           // 0x02  must be 0x10
  OutputBody body_;      // 0x03..0x31
  DSUINT8 reserved4[24]; // 0x32..0x49
  DSUINT32 crc;          // 0x4a  CRC-32, seed 0xA2, little-endian

public:
  enum : uint8_t {
    ReportId = static_cast<uint8_t>(Packet::PacketStateBluetooth),
    Size = 78,
    CrcSeed = 0xA2,
    Tag = 0x10,
  };

  // 0x31 sequence tag: high nibble counts 0..15, low nibble stays zero.
  // The class owns the shift and wrap; callers just hold a Sequence.
  struct Sequence {
    constexpr uint8_t next() {
      uint8_t next_tag = static_cast<uint8_t>((value & Mask) << Shift);
      value = static_cast<uint8_t>((value + 1) & Mask);
      return next_tag;
    }

  private:
    uint8_t value = 0;
    static constexpr uint8_t Mask = 0x0F;
    static constexpr uint8_t Shift = 4;
  };

  constexpr void init() {
    *this = OutputReport{};
    report_id = ReportId;
    tag = Tag;
    seq_tag = 0;
    body_.init();
  }

  constexpr void init(Sequence &sequence) {
    init();
    seq_tag = sequence.next();
  }

  constexpr const OutputBody &body() const { return body_; }

  constexpr OutputBody &body() { return body_; }

  constexpr void sign() { crc = internal::crc_of(*this); }

  constexpr bool valid() const { return report_id == ReportId && crc == internal::crc_of(*this); }
};

static_assert(sizeof(OutputReport) == OutputReport::Size, "0x31 must stay 78 bytes");

// Output 0x02 (USB, 63 bytes). No CRC — the kernel signs only the Bluetooth
// transport — and the same 47-byte body; sizes from hid-playstation.c:
// DS_OUTPUT_REPORT_USB_SIZE 63, DS_OUTPUT_REPORT_BT_SIZE 78.
struct UsbOutputReport {
private:
  using DSUINT8 = internal::DSUINT8;

protected:
  DSUINT8 report_id;    // 0x00  Packet::PacketOutputUsb
  OutputBody body_;     // 0x01..0x2f
  DSUINT8 reserved[15]; // 0x30..0x3e

public:
  enum : uint8_t { ReportId = static_cast<uint8_t>(Packet::PacketOutputUsb), Size = 63 };

  constexpr void init() {
    *this = UsbOutputReport{};
    report_id = ReportId;
    // USB field reports must not claim the audio defaults.
    body_ = OutputBody{};
  }

  constexpr const OutputBody &body() const { return body_; }

  constexpr OutputBody &body() { return body_; }
};

static_assert(sizeof(UsbOutputReport) == UsbOutputReport::Size, "0x02 must stay 63 bytes");

// ===========================================================================
// Input: the 63-byte body shared by 0x31 (Bluetooth) and 0x01 (USB)
// ===========================================================================

// The 63-byte input payload shared by USB (0x01) and Bluetooth (0x31).
//
// `+0x00` in these comments is the offset inside this payload: 0x01 in a USB
// report, 0x02 in a Bluetooth one. The kernel parses both with a single
// `struct dualsense_input_report`, pointing at &data[1] (USB) or &data[2]
// (Bluetooth); this struct is the same body, and the two packet wrappers below
// splice it in.
//
// When microphone duplex is on, not every 0x31 frame is a gamepad report: the
// controller interleaves audio frames that keep the report id but put the Opus
// TOC 0xD4 at report byte 3 (payload +0x01, i.e. where ly normally is). Demux
// on that byte before trusting any of these fields; MicFrame is the schema for
// those frames.
//
// Every byte is named, either with what it does or with an honest reserved/
// feedback name, and every multi-byte integer is little-endian. Sizes come from
// hid-playstation.c: DS_INPUT_REPORT_USB_SIZE 64, DS_INPUT_REPORT_BT_SIZE 78.
struct InputBody {
private:
  using DSUINT8 = internal::DSUINT8;
  using DSUINT32 = internal::DSUINT32;
  using DSINT16 = internal::DSINT16;

public:
  // One 4-byte touch contact on the wire. The high nibble of `xy_high` is
  // y-low; the low nibble is x-high. Bit 7 of `contact` means "not touching".
  struct TouchPoint {
  private:
    DSUINT8 contact;
    DSUINT8 x_low;
    DSUINT8 xy_high;
    DSUINT8 y_high;

  public:
    constexpr bool active() const { return (contact & 0x80) == 0; }

    constexpr unsigned id() const { return contact & 0x7F; }

    constexpr unsigned x() const {
      return static_cast<unsigned>(x_low) | (static_cast<unsigned>(xy_high & 0x0F) << 8);
    }

    constexpr unsigned y() const {
      return static_cast<unsigned>(xy_high >> 4) | (static_cast<unsigned>(y_high) << 4);
    }
  };

  struct TouchData {
    TouchPoint fingers[2];
    uint8_t timestamp;
  };

  struct TriggerFeedback {
    uint8_t value;

    constexpr unsigned stopLocation() const { return value & 0x0F; }

    constexpr unsigned status() const { return value >> 4; }
  };

protected:
  DSUINT8 lx;                 // +0x00  left stick X, 0..255 (128 = centre)
  DSUINT8 ly;                 // +0x01  left stick Y, 0 = up
  DSUINT8 rx;                 // +0x02  right stick X
  DSUINT8 ry;                 // +0x03  right stick Y
  DSUINT8 l2;                 // +0x04  L2 analogue
  DSUINT8 r2;                 // +0x05  R2 analogue
  DSUINT8 seq;                // +0x06  report sequence number
  DSUINT32 buttons_;          // +0x07  Buttons bits (d-pad in bits 0-3)
  DSUINT8 incremental;        // +0x0b  increments every report, wraps at 256
  DSUINT8 reserved_14[3];     // +0x0c  constant (0x84, ?, 0x85) except that the
                              //        middle byte ticks up by one every few
                              //        minutes — a slow counter
  DSINT16 gyro_x;             // +0x0f  angular velocity, little-endian
  DSINT16 gyro_y;             // +0x11
  DSINT16 gyro_z;             // +0x13
  DSINT16 accel_x;            // +0x15  acceleration, ~8192 counts per g
  DSINT16 accel_y;            // +0x17
  DSINT16 accel_z;            // +0x19
  DSUINT32 sensor_time;       // +0x1b  timestamp, 0.33 us units
  DSUINT8 temperature_;       // +0x1f  controller temperature, degrees C
  TouchData touch_data;          // +0x20  two contacts + timestamp
  TriggerFeedback trigger_right; // +0x29  stop location + status
  TriggerFeedback trigger_left;  // +0x2a
  DSUINT32 host_timestamp;       // +0x2b  echoed host timestamp
  DSUINT8 trigger_effect;        // +0x2f  low nibble right, high nibble left
  DSUINT32 device_timestamp;     // +0x30  device clock
  DSUINT8 status0;               // +0x34  low nibble battery, high power state
  DSUINT8 status1;               // +0x35  Status1 bits
  DSUINT8 status2;               // +0x36  Status2 bits
  DSUINT8 aes_cmac[8];        // +0x37  high entropy in every report, like a
                              //        per-report MAC/nonce; not verified
                              //        against the pairing handshake

  static constexpr uint32_t DPAD_MASK = 0x0F;

public:
  enum : uint8_t { Size = 63 };

  struct Stick {
    int x;
    int y;
  };

  struct Triggers {
    int left;
    int right;
  };

  static constexpr int TouchpadWidth = 1920;
  static constexpr int TouchpadHeight = 1080;

  Stick leftStick() const { return {lx, ly}; }

  Stick rightStick() const { return {rx, ry}; }

  Triggers triggers() const { return {l2, r2}; }

  int sequence() const { return seq; }

  // Normalized full-scale angular rate: -1..1.
  struct Gyro {
    float x, y, z;

    constexpr Gyro(int raw_x, int raw_y, int raw_z)
        : x(static_cast<float>(raw_x) / CountsPerFullScale),
          y(static_cast<float>(raw_y) / CountsPerFullScale),
          z(static_cast<float>(raw_z) / CountsPerFullScale) {}

  private:
    static constexpr float CountsPerFullScale = 32768.f;
  };

  Gyro gyro() const { return {gyro_x, gyro_y, gyro_z}; }

  // Acceleration in g: ~8192 raw counts per g.
  struct Accelerometer {
    float x, y, z;

    constexpr Accelerometer(int raw_x, int raw_y, int raw_z)
        : x(static_cast<float>(raw_x) / CountsPerG), y(static_cast<float>(raw_y) / CountsPerG),
          z(static_cast<float>(raw_z) / CountsPerG) {}

    float len() const { return sqrtf(x * x + y * y + z * z); }

  private:
    static constexpr float CountsPerG = 8192.f;
  };

  Accelerometer accelerometer() const { return {accel_x, accel_y, accel_z}; }

  uint8_t temperature() const { return temperature_; }

  // The d-pad is a 4-bit *hat value*, not four bits. The enum turns it into a
  // name; diagonals are the odd values.
  enum class Dpad : uint8_t {
    Up = 0,
    UpRight = 1,
    Right = 2,
    DownRight = 3,
    Down = 4,
    DownLeft = 5,
    Left = 6,
    UpLeft = 7,
    None = 8,
  };

  Dpad dpad() const { return static_cast<Dpad>(static_cast<uint32_t>(buttons_) & DPAD_MASK); }

  // Four button bytes merged into one little-endian 32-bit field: bits 0..7
  // are the byte at +0x07, 8..15 at +0x08, and so on. Bit positions:
  //   +0x07  bits 0-3 d-pad hat (Dpad), 4 square, 5 cross, 6 circle, 7 triangle
  //   +0x08  bits 0 l1, 1 r1, 2 l2, 3 r2, 4 create, 5 options, 6 l3, 7 r3
  //   +0x09  bits 0 ps, 1 touchpad, 2 mute, 3 reserved, 4 fn1, 5 fn2,
  //          6 left_paddle, 7 right_paddle
  //   +0x0a  eight bits the kernel does not read; maybe L5/R5
  enum class Button : uint32_t {
    // +0x07
    ButtonSquare = 1u << 4,
    ButtonCross = 1u << 5,
    ButtonCircle = 1u << 6,
    ButtonTriangle = 1u << 7,
    // +0x08
    L1 = 1u << 8,
    R1 = 1u << 9,
    L2 = 1u << 10,
    R2 = 1u << 11,
    Create = 1u << 12,
    Options = 1u << 13,
    L3 = 1u << 14,
    R3 = 1u << 15,
    // +0x09
    PsHome = 1u << 16,
    Touchpad = 1u << 17,
    MicMute = 1u << 18,
    // bit 19 reserved
    EdgeFn1 = 1u << 20,         // Edge only
    EdgeFn2 = 1u << 21,         // Edge only
    EdgeLeftPaddle = 1u << 22,  // Edge only
    EdgeRightPaddle = 1u << 23, // Edge only
    // +0x0a: unread by the kernel; maybe L5/R5
  };

  struct Buttons {
    friend struct InputBody;

    constexpr Buttons() : buttons(0) {}

    constexpr bool operator!=(const Buttons &o) const { return buttons != o.buttons; }

    constexpr bool isOn(Button b) const { return (buttons & static_cast<uint32_t>(b)) != 0; }

    const Buttons pressed(const Buttons &prev) const {
      return Buttons(static_cast<uint32_t>(buttons) & ~static_cast<uint32_t>(prev.buttons));
    }

  private:
    constexpr Buttons(uint32_t v) : buttons(v) {}

    uint32_t buttons;
  };

  constexpr const Buttons buttons() const { return Buttons(this->buttons_ & ~DPAD_MASK); }

  // status0's low nibble is a coarse 0..10 battery level (8 -> 80%); its
  // high nibble is PowerState. The controller can report Complete before the
  // level reaches 10, especially on an aged pack.
  constexpr int battery() const {
    int percent = (status0 & 0x0F) * 10;
    return percent > 100 ? 100 : percent;
  }

  enum class PowerState : uint8_t {
    Discharging = 0x00,
    Charging = 0x01,
    Complete = 0x02,
    AbnormalVoltage = 0x0A,
    AbnormalTemperature = 0x0B,
    ChargingError = 0x0F,
  };

  constexpr PowerState powerState() const { return static_cast<PowerState>(status0 >> 4); }

  constexpr PowerState charging() const { return powerState(); }

  // +0x35. PluggedUsbData/Power are the two bits observed during USB/charger
  // testing; the kernel only documents the headset/mic/mute bits.
  enum class Status1 : uint8_t {
    HeadphoneDetect = 1u << 0, // bit 0
    MicDetect = 1u << 1,       // bit 1
    MicMute = 1u << 2,         // bit 2
    PluggedUsbData = 1u << 3,  // bit 3
    PluggedUsbPower = 1u << 4, // bit 4
  };

  // +0x36. duaLib names these two; the remaining bits are still unknown.
  enum class Status2 : uint8_t {
    PluggedExternalMic = 1u << 0,  // bit 0
    HapticLowPassFilter = 1u << 1, // bit 1
  };

  constexpr bool headset() const {
    return (status1 & static_cast<uint8_t>(Status1::HeadphoneDetect)) != 0;
  }

  constexpr bool pluggedUsbData() const {
    return (status1 & static_cast<uint8_t>(Status1::PluggedUsbData)) != 0;
  }

  constexpr bool pluggedUsbPower() const {
    return (status1 & static_cast<uint8_t>(Status1::PluggedUsbPower)) != 0;
  }

  constexpr bool microphone() const {
    return (status1 & static_cast<uint8_t>(Status1::MicDetect)) != 0;
  }

  constexpr bool micMute() const { return (status1 & static_cast<uint8_t>(Status1::MicMute)) != 0; }

  constexpr bool pluggedExternalMic() const {
    return (status2 & static_cast<uint8_t>(Status2::PluggedExternalMic)) != 0;
  }

  constexpr bool hapticLowPassFilter() const {
    return (status2 & static_cast<uint8_t>(Status2::HapticLowPassFilter)) != 0;
  }

  TouchPoint touch(int index) const { return touch_data.fingers[index == 0 ? 0 : 1]; }

  uint8_t touchTimestamp() const { return touch_data.timestamp; }

  unsigned triggerFeedbackLeft() const { return trigger_left.value; }

  unsigned triggerFeedbackRight() const { return trigger_right.value; }

  unsigned triggerStopLocationLeft() const { return trigger_left.stopLocation(); }

  unsigned triggerStatusLeft() const { return trigger_left.status(); }

  unsigned triggerStopLocationRight() const { return trigger_right.stopLocation(); }

  unsigned triggerStatusRight() const { return trigger_right.status(); }

  unsigned triggerEffectLeft() const { return trigger_effect >> 4; }

  unsigned triggerEffectRight() const { return trigger_effect & 0x0F; }

  uint32_t hostTimestamp() const { return host_timestamp; }

  uint32_t deviceTimestamp() const { return device_timestamp; }
};

static_assert(sizeof(InputBody::TouchPoint) == 4, "touch point must stay 4 bytes");
static_assert(sizeof(InputBody::TouchData) == 9, "touch data must stay 9 bytes");
static_assert(sizeof(InputBody::TriggerFeedback) == 1, "trigger feedback must stay 1 byte");

static_assert(sizeof(InputBody) == InputBody::Size, "input body must stay 63 bytes");

// ===========================================================================
// Input 0x31 (Bluetooth, 78 bytes) and 0x01 (USB, 64 bytes)
//
// The Bluetooth report is the USB one plus a sequence tag in front and a
// nine-byte tail plus a CRC behind, so every shared field's USB offset is its
// Bluetooth offset minus one. The tail's first byte is not constant (30
// distinct values in a 150 s capture), the other eight were; what it carries is
// unknown. Sizes come from hid-playstation.c (DS_INPUT_REPORT_USB_SIZE 64,
// DS_INPUT_REPORT_BT_SIZE 78); the CRC covers the report with seed 0xA1.
// ===========================================================================

struct InputReport {
private:
  using DSUINT8 = internal::DSUINT8;
  using DSUINT32 = internal::DSUINT32;

protected:
  DSUINT8 report_id;      // 0x00  Packet::PacketStateBluetooth
  DSUINT8 seq_tag;        // 0x01  high nibble = sequence tag, low nibble = 0
  InputBody body_;        // 0x02..0x40
  DSUINT8 bt_reserved[9]; // 0x41..0x49  the first byte is not constant (30
                          //             distinct values in a 150 s capture), the
                          //             other eight were; unknown
  DSUINT32 crc;           // 0x4a  CRC-32 over the report, seed 0xA1

public:
  enum : uint8_t {
    ReportId = static_cast<uint8_t>(Packet::PacketStateBluetooth),
    UsbReportId = static_cast<uint8_t>(Packet::PacketInputUsb),
    Size = 78,
    UsbSize = 64,
    CrcSeed = 0xA1, // the input stream signs with 0xA1, output with 0xA2
  };

  constexpr const InputBody &body() const { return body_; }

  constexpr InputBody &body() { return body_; }

  constexpr void sign() { crc = internal::crc_of(*this); }

  constexpr bool valid() const { return crc == internal::crc_of(*this); }
};

static_assert(sizeof(InputReport) == InputReport::Size, "input 0x31 must stay 78 bytes");

// ===========================================================================
// Audio 0x36 (Bluetooth, 398 bytes)
// ===========================================================================
//
// The undocumented Bluetooth speaker path. Sony says the speaker is USB-only;
// over Bluetooth the controller speaks a proprietary Opus-in-HID protocol that
// LinuxAudio4Dualsense5 reverse-engineered and streams at 93.75 Hz (one packet
// every 512/48000 s). Layout, verified against reverse_engineered/
// ps5bt_membrane.py and the protocol tables:
//
//   0x000  report id 0x36
//   0x001  sequence tag, high nibble (a counter separate from 0x31)
//   0x002  sub-packet 0x11 | 0x80 — audio configuration, length 7
//   0x004  enable mask: bit 0 = microphone capture (0xFE shut, 0xFF open)
//   0x005  5 × haptic block length (64)
//   0x00a  free-running 8-bit counter
//   0x00b  sub-packet 0x10 | 0x80 — state snapshot, length 63
//   0x04c  sub-packet 0x12 | 0x80 — haptics PCM, length 64
//   0x08e  sub-packet 0x13 | 0x80 — speaker Opus (0x16 = headphone jack), 200
//   0x158  50 bytes of padding
//   0x18a  CRC-32, seed 0xA2, little-endian (the same scheme as 0x31)
//
// Three details are the whole difficulty: the 10.667 ms frame interval (not
// 10 ms), Opus CBR 160 kbps, and enable-mask bit 0, which turns on microphone
// capture and floods the link if left set.
//
// The microphone comes back inside the normal 0x31 input report (marker 0xD4,
// Opus 48 kHz mono, 10 ms) when duplex is enabled; see MicFrame below for the
// full recipe. That is why the state snapshot must not mute the mic when
// capture is wanted.
//
// The state snapshot is a whole output payload: its first 47 bytes are
// byte-for-byte OutputBody followed by sixteen bytes of padding, so this
// stream re-states the controller's output configuration 93 times a second and
// every field it does not own must have its valid bit clear or it silently
// overwrites another writer. The cancelled-trigger bug was exactly this: the
// snapshot claimed both trigger effects (0xFD) with a zeroed block; sending
// 0xF1 (volumes and routing only) fixed it. The lightbar was never affected
// because the high byte (0xE3) does not claim the LED colour bit.

struct AudioReport {
  friend struct internal::AudioReportProbe;

private:
  using DSUINT8 = internal::DSUINT8;
  using DSINT8 = internal::DSINT8;
  using DSUINT32 = internal::DSUINT32;
  using DSINT16 = internal::DSINT16;

  // The state snapshot is the 0x31 output payload plus sixteen bytes of
  // padding. Note valid_flag0: claiming RightTrigger or LeftTrigger here hands
  // the trigger effect to this stream, which writes zeros to it 93 times a
  // second. The fix for the cancelled trigger effects was clearing exactly
  // those two bits (0xFD -> 0xF1).
  //
  // This nested type has to precede the field that uses it; everything else in
  // the class can follow its fields.
  struct State {
    friend struct AudioReport;

  protected:
    OutputBody body;     // +0x00..0x2e
    DSUINT8 padding[16]; // +0x2f..0x3e  the rest of the 63-byte block
  };

protected:
  DSUINT8 report_id;         // 0x000  Packet::PacketAudio
  DSUINT8 seq_tag;           // 0x001  high nibble = counter, low nibble = 0
  DSUINT8 config_tag;        // 0x002  0x11 | 0x80 (more sub-packets follow)
  DSUINT8 config_length;     // 0x003  7
  DSUINT8 enable_mask;       // 0x004  EnableMask bits
  DSUINT8 haptic_lengths[5]; // 0x005..0x009  64 each
  DSUINT8 counter;           // 0x00a  free-running 8-bit counter
  DSUINT8 state_tag;         // 0x00b  0x10 | 0x80
  DSUINT8 state_length;      // 0x00c  63
  State state;               // 0x00d..0x04b
  DSUINT8 haptics_tag;       // 0x04c  0x12 | 0x80
  DSUINT8 haptics_length;    // 0x04d  64
  DSINT8 haptics_pcm[64];    // 0x04e..0x08d  64 x int8 PCM at 6 kHz
  DSUINT8 speaker_tag;       // 0x08e  0x13 = membrane, 0x16 = jack (+0x80)
  DSUINT8 speaker_length;    // 0x08f  200
  DSUINT8 opus[200];         // 0x090..0x157  Opus 48 kHz stereo CBR 160 kbps
  DSUINT8 padding[50];       // 0x158..0x189
  DSUINT32 crc;              // 0x18a..0x18d  seed 0xA2

public:
  // PCM-facing encoder: the rest of padwave never touches libopus.
  struct Encoder {
    ::OpusEncoder *encoder = nullptr;

    Encoder() = default;

    ~Encoder() { close(); }

    Encoder(const Encoder &) = delete;
    Encoder &operator=(const Encoder &) = delete;

    bool valid() const { return encoder != nullptr; }

    bool open(int rate, int channels) {
      close();
      int err = 0;
      encoder = opus_encoder_create(rate, channels, OPUS_APPLICATION_AUDIO, &err);
      if (!encoder || err != OPUS_OK) {
        close();
        return false;
      }
      opus_encoder_ctl(encoder, OPUS_SET_BITRATE(160000));
      opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
      opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
      return true;
    }

    int encode(const float *pcm, int frames, uint8_t *out, int cap) {
      if (!encoder) return -1;
      return opus_encode_float(encoder, pcm, frames, out, cap);
    }

    void close() {
      if (encoder) opus_encoder_destroy(encoder);
      encoder = nullptr;
    }
  };

public:
  enum : uint8_t {
    ReportId = static_cast<uint8_t>(Packet::PacketAudio),
    StateSize = 63,
    HapticSize = 64,
    OpusSize = 200,
    CrcSeed = 0xA2,
  };

  // The Opus side of the speaker block: 480 samples per frame at 48 kHz (10 ms,
  // CBR 160 kbps = 200 bytes) fed from 512-sample decoder blocks.
  enum : uint16_t { PcmFrames = 480, InputBlock = 512, Channels = 2 };

  static constexpr size_t Size = 398;

  // +0x004. Bit 0 is the microphone capture request: 0xFE keeps it shut, 0xFF
  // opens it and the input stream then also carries 0xD4 frames.
  enum class EnableMask : uint8_t {
    MicrophoneCapture = 0x01,
    SpeakerOnly = 0xFE,
    SpeakerAndMic = 0xFF,
  };

  // +0x08e: 0x13 for the membrane speaker, 0x16 for the headphone jack.
  enum class SpeakerTag : uint8_t {
    Speaker = 0x13,
    Jack = 0x16,
  };

  constexpr OutputBody &stateBody() { return state.body; }

  constexpr const OutputBody &stateBody() const { return state.body; }

  constexpr void setSeqTag(uint8_t value) { seq_tag = value; }

  constexpr void setCounter(uint8_t value) { counter = value; }

  // 0x36 sequence tag: high nibble counts 0..15, low nibble stays zero;
  // the counter is a separate free-running byte.
  struct Sequence {
    constexpr void apply(AudioReport &report) {
      report.setSeqTag(static_cast<uint8_t>((seq & Mask) << Shift));
      report.setCounter(counter);
      seq = static_cast<uint8_t>((seq + 1) & Mask);
      counter++;
    }

  private:
    uint8_t seq = 0;
    uint8_t counter = 0;
    static constexpr uint8_t Mask = 0x0F;
    static constexpr uint8_t Shift = 4;
  };

  constexpr EnableMask enableMask() const {
    return static_cast<EnableMask>(static_cast<uint8_t>(enable_mask));
  }

  constexpr SpeakerTag speakerTag() const {
    return static_cast<SpeakerTag>(static_cast<uint8_t>(speaker_tag) & 0x7F);
  }

  // Fill the constant parts, then set seq, counter and whatever state fields
  // this writer owns.
  constexpr void init(SpeakerTag tag, EnableMask mask = EnableMask::SpeakerOnly) {
    *this = AudioReport{};
    report_id = ReportId;
    config_tag = 0x11 | 0x80;
    config_length = 7;
    enable_mask = static_cast<uint8_t>(mask);
    for (int i = 0; i < 5; ++i) haptic_lengths[i] = HapticSize;
    state_tag = 0x10 | 0x80;
    state_length = StateSize;
    haptics_tag = 0x12 | 0x80;
    haptics_length = HapticSize;
    speaker_tag = static_cast<uint8_t>(static_cast<uint8_t>(tag) | 0x80);
    speaker_length = OpusSize;
  }

  bool setPcm(const float *pcm, int frames, Encoder &encoder) {
    uint8_t payload[OpusSize];
    int n = encoder.encode(pcm, frames, payload, static_cast<int>(sizeof(payload)));
    if (n < 0) n = 0;
    setOpus(payload, static_cast<size_t>(n));
    return n > 0;
  }

  constexpr void setOpus(const uint8_t *data, size_t len) {
    size_t n = len < OpusSize ? len : OpusSize;
    for (size_t i = 0; i < n; ++i) opus[i] = data[i];
    for (size_t i = n; i < OpusSize; ++i) opus[i] = 0;
  }

  // 64 signed 8-bit samples at 6 kHz. Zero-fill any unused tail so a short
  // haptic frame cannot leak the previous report's samples.
  constexpr void setHaptics(const int8_t *data, size_t len) {
    size_t n = len < HapticSize ? len : HapticSize;
    for (size_t i = 0; i < n; ++i) haptics_pcm[i] = data[i];
    for (size_t i = n; i < HapticSize; ++i) haptics_pcm[i] = 0;
  }

  constexpr void sign() { crc = internal::crc_of(*this); }

  constexpr bool valid() const { return report_id == ReportId && crc == internal::crc_of(*this); }
};

static_assert(sizeof(AudioReport) == AudioReport::Size, "0x36 must stay 398 bytes");

namespace internal {
struct AudioReportProbe {
  static constexpr uint8_t reportId(const AudioReport &r) { return r.report_id; }

  static constexpr uint8_t seqTag(const AudioReport &r) { return r.seq_tag; }

  static constexpr uint8_t configTag(const AudioReport &r) { return r.config_tag; }

  static constexpr uint8_t configLength(const AudioReport &r) { return r.config_length; }

  static constexpr uint8_t hapticLength(const AudioReport &r, size_t i) {
    return r.haptic_lengths[i];
  }

  static constexpr uint8_t counter(const AudioReport &r) { return r.counter; }

  static constexpr uint8_t stateTag(const AudioReport &r) { return r.state_tag; }

  static constexpr uint8_t stateLength(const AudioReport &r) { return r.state_length; }

  static constexpr uint8_t speakerLength(const AudioReport &r) { return r.speaker_length; }

  static constexpr uint8_t opusByte(const AudioReport &r, size_t i) { return r.opus[i]; }

  static constexpr size_t stateSize() { return sizeof(AudioReport::State); }
};
} // namespace internal

// ===========================================================================
// Feature reports — the configuration channel
// ===========================================================================
//
// Read (and occasionally written) with a HID GET_REPORT/SET_REPORT on report
// ids that are not part of the streaming protocol. The kernel uses 0x05, 0x09
// and 0x20 and the offsets below come from hid-playstation.c; DualSenseClient's
// descriptor notes corroborate the ids and lengths. Feature reports are signed
// with CRC-32 seed 0xA3 over Bluetooth (the streams use 0xA1 input, 0xA2
// output). padwave does not read them — the kernel already exposes the results
// (MAC, battery, calibration) — but they are here so the whole protocol is
// visible.
//
//   id   length  what
//   0x05 41      motion-sensor calibration: 17 int16 LE at buf[1..34]
//   0x09 20      pairing info; MAC at buf[1..6] (some clients read it reversed)
//   0x20 64      firmware/hardware: hw buf[24], fw buf[28], update buf[44]
//   0x80 —       factory command write (Edge/vendor tooling)
//   0x81 64      the matching command response
//   0x65 64      DualSense Edge factory handshake
//
// The Edge also exposes profile reports in the 0x60..0x7B range, beyond what
// padwave needs.

// Calibration math from the Swift schema. The feature report stores raw
// per-axis bias and plus/minus endpoints; converting them to the coefficients
// below is what turns raw IMU counts into usable units.
struct AxisCalibration {
  int bias = 0;
  int numerator = 0;
  int denominator = 1;

  constexpr int apply(int raw) const {
    if (denominator == 0) return 0;
    return numerator * (raw - bias) / denominator;
  }

  static constexpr AxisCalibration fallback(int range) { return {0, range, 32767}; }
};

struct ImuCalibration {
  static constexpr int GyroResolutionPerDps = 1024;
  static constexpr int AccelResolutionPerG = 8192;
  static constexpr int AccelRange = 4 * AccelResolutionPerG;
  static constexpr int GyroRange = 2048 * GyroResolutionPerDps;

  AxisCalibration gyro[3];
  AxisCalibration accel[3];
};

namespace internal {
struct Calibration {
protected:
  DSUINT8 report_id; // 0x00  Packet::PacketFeatureCalibration

  // 0x01..0x22: seventeen little-endian int16: three gyro biases, then
  // plus/minus at 2x speed for each axis, then gyro speed limits, then accel
  // plus/minus at 1 g per axis.
  DSINT16 gyro_pitch_bias;
  DSINT16 gyro_yaw_bias;
  DSINT16 gyro_roll_bias;
  DSINT16 gyro_pitch_plus;
  DSINT16 gyro_pitch_minus;
  DSINT16 gyro_yaw_plus;
  DSINT16 gyro_yaw_minus;
  DSINT16 gyro_roll_plus;
  DSINT16 gyro_roll_minus;
  DSINT16 gyro_speed_plus;
  DSINT16 gyro_speed_minus;
  DSINT16 acc_x_plus;
  DSINT16 acc_x_minus;
  DSINT16 acc_y_plus;
  DSINT16 acc_y_minus;
  DSINT16 acc_z_plus;
  DSINT16 acc_z_minus;
  DSUINT8 unknown_35[6]; // 0x23..0x28  not used by the kernel

public:
  enum : uint8_t { ReportId = static_cast<uint8_t>(Packet::PacketFeatureCalibration), Size = 41 };

  constexpr ImuCalibration imu() const {
    ImuCalibration result{};
    int speed_2x = static_cast<int>(gyro_speed_plus) + static_cast<int>(gyro_speed_minus);
    int gyro_numerator = speed_2x * ImuCalibration::GyroResolutionPerDps;

    int pitch_bias = static_cast<int>(gyro_pitch_bias);
    int yaw_bias = static_cast<int>(gyro_yaw_bias);
    int roll_bias = static_cast<int>(gyro_roll_bias);
    result.gyro[0] = {0, gyro_numerator,
                      static_cast<int>(gyro_pitch_plus) - pitch_bias +
                          static_cast<int>(gyro_pitch_minus) - pitch_bias};
    result.gyro[1] = {0, gyro_numerator,
                      static_cast<int>(gyro_yaw_plus) - yaw_bias +
                          static_cast<int>(gyro_yaw_minus) - yaw_bias};
    result.gyro[2] = {0, gyro_numerator,
                      static_cast<int>(gyro_roll_plus) - roll_bias +
                          static_cast<int>(gyro_roll_minus) - roll_bias};
    for (auto &axis : result.gyro)
      if (axis.denominator == 0) axis = AxisCalibration::fallback(ImuCalibration::GyroRange);

    auto accel_axis = [](int plus, int minus) {
      int range_2g = plus - minus;
      if (range_2g == 0) return AxisCalibration::fallback(ImuCalibration::AccelRange);
      return AxisCalibration{plus - range_2g / 2, 2 * ImuCalibration::AccelResolutionPerG,
                             range_2g};
    };
    result.accel[0] = accel_axis(acc_x_plus, acc_x_minus);
    result.accel[1] = accel_axis(acc_y_plus, acc_y_minus);
    result.accel[2] = accel_axis(acc_z_plus, acc_z_minus);
    return result;
  }
};

static_assert(sizeof(Calibration) == Calibration::Size, "0x05 must stay 41 bytes");

struct Pairing {
protected:
  DSUINT8 report_id;      // 0x00  Packet::PacketFeaturePairing
  DSUINT8 mac[6];         // 0x01..0x06  the controller's own address
  DSUINT8 unknown_07[13]; // 0x07..0x13

public:
  enum : uint8_t { ReportId = static_cast<uint8_t>(Packet::PacketFeaturePairing), Size = 20 };
};

static_assert(sizeof(Pairing) == Pairing::Size, "0x09 must stay 20 bytes");

struct Firmware {
protected:
  DSUINT8 report_id;        // 0x00  Packet::PacketFeatureFirmware
  char build_date[11];      // 0x01..0x0b
  char build_time[8];       // 0x0c..0x13
  DSUINT16 fw_type;         // 0x14..0x15
  DSUINT16 sw_series;       // 0x16..0x17
  DSUINT32 hardware_info;   // 0x18..0x1b  variation/generation
  DSUINT32 firmware_version; // 0x1c..0x1f
  char device_info[12];     // 0x20..0x2b
  DSUINT16 update_version;  // 0x2c..0x2d
  DSUINT8 update_image_info; // 0x2e
  DSUINT8 update_unk;       // 0x2f
  DSUINT32 fw_version1;     // 0x30..0x33  SBL
  DSUINT32 fw_version2;     // 0x34..0x37  Venom
  DSUINT32 fw_version3;     // 0x38..0x3b  Spider DSP
  DSUINT8 padding[4];       // 0x3c..0x3f

public:
  enum : uint8_t {
    ReportId = static_cast<uint8_t>(Packet::PacketFeatureFirmware),
    Size = 64,
  };

  static constexpr uint16_t VibrationV2MinimumVersion = (2 << 8) | 21;

  constexpr uint32_t hardwareInfo() const { return hardware_info; }

  constexpr uint32_t firmwareVersion() const { return firmware_version; }

  constexpr uint16_t updateVersion() const { return update_version; }

  constexpr bool usesVibrationV2() const { return update_version >= VibrationV2MinimumVersion; }
};

static_assert(sizeof(Firmware) == Firmware::Size, "0x20 must stay 64 bytes");
} // namespace internal

// ===========================================================================
// Microphone frames inside the 0x31 input stream
// ===========================================================================
//
// The controller has no audio class over Bluetooth: the microphone is
// multiplexed into the same HID reports as the gamepad, and its audio is Opus,
// not PCM. Byte 3 is the constant Opus TOC 0xD4; a state report has the
// sequence/tag nibbles there instead.
//
//   0x31 seq .. 0xD4 | one Opus frame | CRC-32 (4 bytes, seed 0xA1)
//
// Opus, 48 kHz, mono, 480 samples (10 ms), about 100 packets per second.
//
// Turning capture on takes three writes in the outgoing 0x36:
//   1. power_save_control (+0x09) bit 4 = 0 AND valid_flag1 bit 1 claims the
//      byte (without the claim the unmute is inert);
//   2. enable mask (0x36 byte 4) bit 0 set: 0xFF instead of 0xFE;
//   3. mic_volume (+0x06) = 0x40, claimed by valid_flag0 bit 6.
// Step 2 is the tap; step 1 is the one people forget.
//
// It disturbs the gamepad input: these frames carry the same report id as the
// state report, so anything that assumes every 0x31 is 78 bytes reads Opus
// bytes as sticks and buttons. The kernel ignores them, so there is no ALSA
// capture device; decoding them is a host-side job. Duplex roughly doubles the
// link traffic (speaker down and mic up are each ~160 kbps of Opus).

struct MicFrame {
  // PCM-facing decoder for the Opus mic packet.
  struct Decoder {
    ::OpusDecoder *decoder = nullptr;

    Decoder() = default;

    ~Decoder() { close(); }

    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    bool valid() const { return decoder != nullptr; }

    bool open(int rate, int channels) {
      close();
      int err = 0;
      decoder = opus_decoder_create(rate, channels, &err);
      if (!decoder || err != OPUS_OK) {
        close();
        return false;
      }
      return true;
    }

    int decode(const uint8_t *data, size_t len, int16_t *pcm, int max_samples) {
      if (!decoder) return -1;
      return opus_decode(decoder, data, static_cast<opus_int32>(len), pcm, max_samples, 0);
    }

    void close() {
      if (decoder) opus_decoder_destroy(decoder);
      decoder = nullptr;
    }
  };

  enum : uint8_t {
    ReportId = static_cast<uint8_t>(Packet::PacketStateBluetooth),
    Toc = 0xD4,
    CrcBytes = 4,
  };

  enum : uint32_t {
    SampleRate = 48000,
    Samples = 480,
    FrameMs = 10,
  };

  static bool is_frame(const uint8_t *buf, size_t len) {
    return len > 3 + CrcBytes && buf[0] == ReportId && buf[3] == Toc;
  }

  // The Opus packet: everything after the TOC byte, minus the trailing CRC.
  static const uint8_t *payload(const uint8_t *buf, size_t len, size_t *out_len) {
    if (!is_frame(buf, len)) return nullptr;
    *out_len = len - 3 - CrcBytes;
    return buf + 3;
  }
};

// ===========================================================================
// Compile-time wire checks
// ===========================================================================
//
// These static_asserts are part of the header, so every translation unit that
// uses the schema validates endianness, offsets, initialized fields and CRC.
namespace internal {
namespace wire_test {

// --- byte order ------------------------------------------------------------

constexpr DSUINT16 kU16 = [] {
  DSUINT16 v;
  v = 0x1234;
  return v;
}();
static_assert(kU16.bytes[0] == 0x34 && kU16.bytes[1] == 0x12);
static_assert(kU16 == 0x1234);

constexpr DSUINT32 kU32 = [] {
  DSUINT32 v;
  v = 0x01020304;
  return v;
}();
static_assert(kU32.bytes[0] == 0x04 && kU32.bytes[1] == 0x03 && kU32.bytes[2] == 0x02 &&
              kU32.bytes[3] == 0x01);
static_assert(kU32 == 0x01020304);

constexpr DSINT16 kS16 = [] {
  DSINT16 v;
  v = -2;
  return v;
}();
static_assert(kS16.bytes[0] == 0xFE && kS16.bytes[1] == 0xFF);
static_assert(kS16 == -2);

constexpr DSINT32 kS32 = [] {
  DSINT32 v;
  v = -1;
  return v;
}();
static_assert(kS32.bytes[0] == 0xFF && kS32.bytes[3] == 0xFF);

// --- CRC-32 ----------------------------------------------------------------

constexpr uint8_t kCheck[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
static_assert(internal::crc32(0, kCheck, sizeof(kCheck)) == 0xCBF43926u);
constexpr uint32_t kChained = [] {
  uint32_t c = internal::crc32(0, kCheck, 4);
  return internal::crc32(c, kCheck + 4, 5);
}();
static_assert(kChained == 0xCBF43926u);

// --- initialized reports and offsets --------------------------------------

constexpr AudioReport kReport = [] {
  AudioReport r{};
  r.init(AudioReport::SpeakerTag::Jack, AudioReport::EnableMask::SpeakerOnly);
  r.stateBody().init(OutputBody::AudioControl::Headphones);
  r.setSeqTag(0x00);
  r.setCounter(0);
  return r;
}();

static_assert(sizeof(AudioReport) == 398);
static_assert(internal::AudioReportProbe::stateSize() == 63);
static_assert(sizeof(OutputBody) == 47);

static_assert(internal::AudioReportProbe::reportId(kReport) == 0x36);
static_assert(internal::AudioReportProbe::seqTag(kReport) == 0x00);
static_assert(internal::AudioReportProbe::configTag(kReport) == 0x91);
static_assert(internal::AudioReportProbe::configLength(kReport) == 7);
static_assert(kReport.enableMask() == AudioReport::EnableMask::SpeakerOnly);
static_assert(internal::AudioReportProbe::hapticLength(kReport, 0) == 64);
static_assert(internal::AudioReportProbe::hapticLength(kReport, 4) == 64);
static_assert(internal::AudioReportProbe::counter(kReport) == 0);
static_assert(internal::AudioReportProbe::stateTag(kReport) == 0x90);
static_assert(internal::AudioReportProbe::stateLength(kReport) == 63);
static_assert(internal::OutputBodyProbe::valid0(kReport.stateBody()) == 0xF0);
static_assert(internal::OutputBodyProbe::valid1(kReport.stateBody()) == 0x80);
static_assert(kReport.stateBody().headphoneVolume() == 0x7F);
static_assert(kReport.stateBody().speakerVolume() == 0x64);
static_assert(kReport.speakerTag() == AudioReport::SpeakerTag::Jack);
static_assert(internal::AudioReportProbe::speakerLength(kReport) == 200);
static_assert(internal::AudioReportProbe::opusByte(kReport, 0) == 0);

// --- sign / validate / byte copy, all constexpr ---------------------------

constexpr AudioReport kSigned = [] {
  AudioReport r = kReport;
  r.sign();
  return r;
}();
static_assert(kSigned.valid());

constexpr auto kRaw = __builtin_bit_cast(internal::WireBytes<AudioReport>, kSigned);
constexpr AudioReport kCopy = __builtin_bit_cast(AudioReport, kRaw);
static_assert(kCopy.valid());

} // namespace wire_test
} // namespace internal
} // namespace dualsense
