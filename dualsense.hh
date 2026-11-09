// dualsense.hh — DualSense HID wire schema for C++23.
//
// Self-contained protocol definitions: report framing, output/input bodies,
// feature reports, haptics and the command metadata. The public report classes
// expose construction/accessors; storage and compile-time probes stay in
// dualsense::internal. No STL, RTTI, exceptions, heap allocation or platform
// I/O. DS_HAVE_OPUS enables the optional Opus packet encoder.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>

#ifdef DS_HAVE_OPUS
#include <opus/opus.h>
#endif

namespace dualsense {
namespace internal {

template <typename Type, typename UnsignedType = Type>
struct NumType {
  constexpr NumType() { *this = Type{}; }

  constexpr explicit NumType(Type value) { *this = value; }

  constexpr NumType &operator=(Type value) {
    UnsignedType v = static_cast<UnsignedType>(value);
    for (unsigned i = 0; i < sizeof(Type); ++i) {
      bytes[i] = static_cast<unsigned char>((v & 0xFF));
      v        = static_cast<UnsignedType>((v >> 8));
    }
    return *this;
  }

  constexpr operator Type() const {
    UnsignedType v = 0;
    for (unsigned i = 0; i < sizeof(Type); ++i)
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

  uint8_t bytes[sizeof(Type)];
};

using DSUINT8  = NumType<uint8_t>;
using DSUINT16 = NumType<uint16_t>;
using DSUINT32 = NumType<uint32_t>;
using DSINT8   = NumType<int8_t, uint8_t>;
using DSINT16  = NumType<int16_t, uint16_t>;
using DSINT32  = NumType<int32_t, uint32_t>;

static_assert(sizeof(DSUINT8) == 1, "DSUINT8 is one byte");
static_assert(sizeof(DSUINT16) == 2, "DSUINT16 is two bytes");
static_assert(sizeof(DSUINT32) == 4, "DSUINT32 is four bytes");
static_assert(sizeof(DSINT8) == 1, "DSINT8 is one byte");
static_assert(sizeof(DSINT16) == 2, "DSINT16 is two bytes");
static_assert(sizeof(DSINT32) == 4, "DSINT32 is four bytes");

// The CRC the controller checks: standard CRC-32 (IEEE 802.3, reflected
// polynomial 0xEDB88320), seeded with the stream's seed byte (0xA1 input,
// 0xA2 output, 0xA3 features), over everything before the trailing `crc`
// member. CRC-bearing report types carry `crc`, `crcSeed` and a `reportId`, so
// one template covers them. This is the same result as zlib's crc32(), but
// local so the binary does not need -lz. USB input/output reports do not carry
// a CRC.
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
  uint8_t data[sizeof(Report)];
};

template <typename Report>
static constexpr uint32_t crc_of(const Report &report, uint8_t crcSeed) {
  uint32_t c = crc32(0, &crcSeed, 1);
  auto raw   = __builtin_bit_cast(WireBytes<Report>, report);
  return crc32(c, raw.data, sizeof(Report) - sizeof(DSUINT32));
}

// ===========================================================================
// Output: 47-byte state body shared by 0x31 and 0x02. Valid bits claim fields;
// clear bits leave them untouched.
// ===========================================================================

// Report ordinals, common to every schema. 0x31 is both the input and the
// output state report; the direction is the stream, not the id.

enum class ReportId : uint8_t {
  InputUsb           = 0x01,
  OutputUsb          = 0x02,
  FeatureCalibration = 0x05,
  FeaturePairing     = 0x09,
  FeatureFirmware    = 0x20,
  StateBluetooth     = 0x31,
  Haptics            = 0x32,
  Audio              = 0x36,
  CommandHost        = 0x80,
  CommandStatus      = 0x81,
  FeatureMode        = 0x08,
  FeatureSetup       = 0x0B,
  Command82          = 0x82,
  Command83          = 0x83,
  Command84          = 0x84,
  Command85          = 0x85,
  CommandGyro        = 0x90,
  CommandGyroStatus  = 0x91,
  CommandCodec       = 0xA0,
  CommandQuery       = 0xA4,
  CommandTable       = 0xA5,
  CommandFlash       = 0xF0,
  AuthChallenge      = 0xF0, // same ID observed as the authentication challenge
  AuthResponse       = 0xF1,
  AuthStatus         = 0xF2,
};

struct WireProbe;
} // namespace internal

struct OutputBody {
  friend struct internal::WireProbe;

public:
  // Canonical 11-byte adaptive-trigger block: type byte followed by ten
  // parameters. Encoders adapted from ExtendInput.DataTools.DualSense
  // TriggerEffectGenerator (revision 6) by John "Nielk1" Klein, MIT License.
  struct TriggerEffect {
    friend struct internal::WireProbe;
    friend struct OutputBody;

  private:
    enum class Mode : uint8_t {
      Invalid         = 0x00,
      Off             = 0x05,
      Feedback        = 0x21,
      Weapon          = 0x25,
      Vibration       = 0x26,
      Bow             = 0x22,
      Galloping       = 0x23,
      Machine         = 0x27,
      SimpleFeedback  = 0x01,
      SimpleWeapon    = 0x02,
      SimpleVibration = 0x06,
      LimitedFeedback = 0x11,
      LimitedWeapon   = 0x12,
      DebugFc         = 0xFC,
      DebugFd         = 0xFD,
      DebugFe         = 0xFE,
    };

    Mode mode;
    uint8_t parameters[10];

    constexpr explicit TriggerEffect(Mode modeValue = Mode::Invalid)
        : mode(modeValue), parameters() {}

    // Apple-style normalized [0,1] adapters from the same generator.
    static constexpr uint8_t normalize(float value, float scale) {
      if (!(value > -1.0e30f && value < 1.0e30f)) return 0;
      if (value < 0) value = 0;
      if (value > 1) value = 1;
      int scaled = static_cast<int>(value * scale + 0.5f);
      return static_cast<uint8_t>(scaled);
    }

  public:
    constexpr bool isValid() const { return this->mode != Mode::Invalid; }

    static constexpr const TriggerEffect off() { return TriggerEffect(Mode::Off); }

    // Official: constant resistance from `position` to the end.
    static constexpr const TriggerEffect feedback(uint8_t position, uint8_t strength) {
      if (position > 9 || strength > 8) return TriggerEffect{};
      if (strength == 0) return off();
      uint16_t active = 0;
      uint32_t force  = 0;
      uint8_t value   = static_cast<uint8_t>((strength - 1) & 0x07);
      for (unsigned i = position; i < 10; ++i) {
        active = static_cast<uint16_t>(active | (uint16_t{1} << i));
        force |= static_cast<uint32_t>(value) << (3 * i);
      }
      TriggerEffect effect(Mode::Feedback);
      effect.parameters[0] = static_cast<uint8_t>(active & 0xFF);
      effect.parameters[1] = static_cast<uint8_t>(active >> 8);
      effect.parameters[2] = static_cast<uint8_t>(force & 0xFF);
      effect.parameters[3] = static_cast<uint8_t>((force >> 8) & 0xFF);
      effect.parameters[4] = static_cast<uint8_t>((force >> 16) & 0xFF);
      effect.parameters[5] = static_cast<uint8_t>((force >> 24) & 0xFF);
      return effect;
    }

    // Official: resistance between start and end, releasing past the end.
    static constexpr const TriggerEffect weapon(uint8_t start, uint8_t end, uint8_t strength) {
      if (start > 7 || start < 2 || end > 8 || end <= start || strength > 8) return TriggerEffect{};
      if (strength == 0) return off();
      uint16_t zones = static_cast<uint16_t>((uint16_t{1} << start) | (uint16_t{1} << end));
      TriggerEffect effect(Mode::Weapon);
      effect.parameters[0] = static_cast<uint8_t>(zones & 0xFF);
      effect.parameters[1] = static_cast<uint8_t>(zones >> 8);
      effect.parameters[2] = static_cast<uint8_t>(strength - 1);
      return effect;
    }

    // Official: vibration from `position` onward.
    static constexpr const TriggerEffect vibration(uint8_t position, uint8_t amplitude,
                                                   uint8_t frequency) {
      if (position > 9 || amplitude > 8) return TriggerEffect{};
      if (amplitude == 0 || frequency == 0) return off();
      uint16_t active = 0;
      uint32_t levels = 0;
      uint8_t value   = static_cast<uint8_t>((amplitude - 1) & 0x07);
      for (unsigned i = position; i < 10; ++i) {
        active = static_cast<uint16_t>(active | (uint16_t{1} << i));
        levels |= static_cast<uint32_t>(value) << (3 * i);
      }
      TriggerEffect effect(Mode::Vibration);
      effect.parameters[0] = static_cast<uint8_t>(active & 0xFF);
      effect.parameters[1] = static_cast<uint8_t>(active >> 8);
      effect.parameters[2] = static_cast<uint8_t>(levels & 0xFF);
      effect.parameters[3] = static_cast<uint8_t>((levels >> 8) & 0xFF);
      effect.parameters[4] = static_cast<uint8_t>((levels >> 16) & 0xFF);
      effect.parameters[5] = static_cast<uint8_t>((levels >> 24) & 0xFF);
      effect.parameters[8] = frequency;
      return effect;
    }

    // Official: per-zone resistance levels (0..8).
    static constexpr const TriggerEffect multiplePositionFeedback(const uint8_t strength[10]) {
      bool any = false;
      for (unsigned i = 0; i < 10; ++i)
        if (strength[i] > 0) any = true;
      if (!any) return off();
      uint16_t active = 0;
      uint32_t force  = 0;
      for (unsigned i = 0; i < 10; ++i) {
        if (strength[i] == 0) continue;
        uint8_t value = static_cast<uint8_t>((strength[i] - 1) & 0x07);
        active        = static_cast<uint16_t>(active | (uint16_t{1} << i));
        force |= static_cast<uint32_t>(value) << (3 * i);
      }
      TriggerEffect effect(Mode::Feedback);
      effect.parameters[0] = static_cast<uint8_t>(active & 0xFF);
      effect.parameters[1] = static_cast<uint8_t>(active >> 8);
      effect.parameters[2] = static_cast<uint8_t>(force & 0xFF);
      effect.parameters[3] = static_cast<uint8_t>((force >> 8) & 0xFF);
      effect.parameters[4] = static_cast<uint8_t>((force >> 16) & 0xFF);
      effect.parameters[5] = static_cast<uint8_t>((force >> 24) & 0xFF);
      return effect;
    }

    // Official: linearly changing resistance between two positions.
    static constexpr const TriggerEffect
    slopeFeedback(uint8_t start, uint8_t end, uint8_t start_strength, uint8_t end_strength) {
      if (start > 8 || end > 9 || end <= start) return TriggerEffect{};
      if (start_strength < 1 || start_strength > 8 || end_strength < 1 || end_strength > 8)
        return TriggerEffect{};
      uint8_t strength[10] = {};
      float slope          = static_cast<float>(static_cast<int>(end_strength) - start_strength) /
                             static_cast<float>(end - start);
      for (unsigned i = start; i < 10; ++i) {
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
    static constexpr const TriggerEffect multiplePositionVibration(uint8_t frequency,
                                                                   const uint8_t amplitude[10]) {
      bool any = false;
      for (unsigned i = 0; i < 10; ++i)
        if (amplitude[i] > 0) any = true;
      if (frequency == 0 || !any) return off();
      uint16_t active = 0;
      uint32_t levels = 0;
      for (unsigned i = 0; i < 10; ++i) {
        if (amplitude[i] == 0) continue;
        uint8_t value = static_cast<uint8_t>((amplitude[i] - 1) & 0x07);
        active        = static_cast<uint16_t>(active | (uint16_t{1} << i));
        levels |= static_cast<uint32_t>(value) << (3 * i);
      }
      TriggerEffect effect(Mode::Vibration);
      effect.parameters[0] = static_cast<uint8_t>(active & 0xFF);
      effect.parameters[1] = static_cast<uint8_t>(active >> 8);
      effect.parameters[2] = static_cast<uint8_t>(levels & 0xFF);
      effect.parameters[3] = static_cast<uint8_t>((levels >> 8) & 0xFF);
      effect.parameters[4] = static_cast<uint8_t>((levels >> 16) & 0xFF);
      effect.parameters[5] = static_cast<uint8_t>((levels >> 24) & 0xFF);
      effect.parameters[8] = frequency;
      return effect;
    }

    // Unofficial firmware effects.
    static constexpr const TriggerEffect bow(uint8_t start, uint8_t end, uint8_t strength,
                                             uint8_t snap) {
      if (start > 8 || end > 8 || start >= end || strength > 8 || snap > 8) return TriggerEffect{};
      if (end == 0 || strength == 0 || snap == 0) return off();
      uint16_t zones = static_cast<uint16_t>((uint16_t{1} << start) | (uint16_t{1} << end));
      uint32_t pair  = static_cast<uint32_t>((strength - 1) & 0x07) |
                       (static_cast<uint32_t>((snap - 1) & 0x07) << 3);
      TriggerEffect effect(Mode::Bow);
      effect.parameters[0] = static_cast<uint8_t>(zones & 0xFF);
      effect.parameters[1] = static_cast<uint8_t>(zones >> 8);
      effect.parameters[2] = static_cast<uint8_t>(pair & 0xFF);
      effect.parameters[3] = static_cast<uint8_t>((pair >> 8) & 0xFF);
      return effect;
    }

    static constexpr const TriggerEffect galloping(uint8_t start, uint8_t end, uint8_t first_foot,
                                                   uint8_t second_foot, uint8_t frequency) {
      if (start > 8 || end > 9 || start >= end || second_foot > 7 || first_foot > 6 ||
          first_foot >= second_foot)
        return TriggerEffect{};
      if (frequency == 0) return off();
      uint16_t zones = static_cast<uint16_t>((uint16_t{1} << start) | (uint16_t{1} << end));
      uint32_t ratio = static_cast<uint32_t>(second_foot & 0x07) |
                       (static_cast<uint32_t>(first_foot & 0x07) << 3);
      TriggerEffect effect(Mode::Galloping);
      effect.parameters[0] = static_cast<uint8_t>(zones & 0xFF);
      effect.parameters[1] = static_cast<uint8_t>(zones >> 8);
      effect.parameters[2] = static_cast<uint8_t>(ratio & 0xFF);
      effect.parameters[3] = frequency;
      return effect;
    }

    static constexpr const TriggerEffect machine(uint8_t start, uint8_t end, uint8_t amplitude_a,
                                                 uint8_t amplitude_b, uint8_t frequency,
                                                 uint8_t period) {
      if (start > 8 || end > 9 || end <= start || amplitude_a > 7 || amplitude_b > 7)
        return TriggerEffect{};
      if (frequency == 0) return off();
      uint16_t zones = static_cast<uint16_t>((uint16_t{1} << start) | (uint16_t{1} << end));
      uint8_t pair   = static_cast<uint8_t>((amplitude_a & 0x07) | ((amplitude_b & 0x07) << 3));
      TriggerEffect effect(Mode::Machine);
      effect.parameters[0] = static_cast<uint8_t>(zones & 0xFF);
      effect.parameters[1] = static_cast<uint8_t>(zones >> 8);
      effect.parameters[2] = pair;
      effect.parameters[3] = frequency;
      effect.parameters[4] = period;
      return effect;
    }

    // Firmware leftovers with simpler logic and no parameter validation.
    static constexpr const TriggerEffect simpleFeedback(uint8_t position, uint8_t strength) {
      TriggerEffect effect(Mode::SimpleFeedback);
      effect.parameters[0] = position;
      effect.parameters[1] = strength;
      return effect;
    }

    static constexpr const TriggerEffect simpleWeapon(uint8_t start, uint8_t end,
                                                      uint8_t strength) {
      TriggerEffect effect(Mode::SimpleWeapon);
      effect.parameters[0] = start;
      effect.parameters[1] = end;
      effect.parameters[2] = strength;
      return effect;
    }

    static constexpr const TriggerEffect simpleVibration(uint8_t position, uint8_t amplitude,
                                                         uint8_t frequency) {
      if (frequency == 0 || amplitude == 0) return off();
      TriggerEffect effect(Mode::SimpleVibration);
      effect.parameters[0] = frequency;
      effect.parameters[1] = amplitude;
      effect.parameters[2] = position;
      return effect;
    }

    // Firmware leftovers with narrower validation ranges.
    static constexpr const TriggerEffect limitedFeedback(uint8_t position, uint8_t strength) {
      if (strength > 10) return TriggerEffect{};
      if (strength == 0) return off();
      TriggerEffect effect(Mode::LimitedFeedback);
      effect.parameters[0] = position;
      effect.parameters[1] = strength;
      return effect;
    }

    static constexpr const TriggerEffect limitedWeapon(uint8_t start, uint8_t end,
                                                       uint8_t strength) {
      if (start < 0x10 || end < start || end > start + 100 || strength > 10) return TriggerEffect{};
      if (strength == 0) return off();
      TriggerEffect effect(Mode::LimitedWeapon);
      effect.parameters[0] = start;
      effect.parameters[1] = end;
      effect.parameters[2] = strength;
      return effect;
    }

    static constexpr const TriggerEffect appleFeedbackWithStartPosition(float start,
                                                                        float strength) {
      return feedback(normalize(start, 9.0f), normalize(strength, 8.0f));
    }

    static constexpr const TriggerEffect appleWeaponWithStartPosition(float start, float end,
                                                                      float strength) {
      return weapon(normalize(start, 9.0f), normalize(end, 9.0f), normalize(strength, 8.0f));
    }

    static constexpr const TriggerEffect
    appleVibrationWithStartPosition(float start, float amplitude, float frequency) {
      return vibration(normalize(start, 9.0f), normalize(amplitude, 8.0f),
                       normalize(frequency, 255.0f));
    }

    static constexpr const TriggerEffect appleFeedback(const float strengths[10]) {
      uint8_t levels[10];
      for (unsigned i = 0; i < 10; ++i) levels[i] = normalize(strengths[i], 8.0f);
      return multiplePositionFeedback(levels);
    }

    static constexpr const TriggerEffect
    appleSlopeFeedback(float start, float end, float start_strength, float end_strength) {
      return slopeFeedback(normalize(start, 9.0f), normalize(end, 9.0f),
                           normalize(start_strength, 8.0f), normalize(end_strength, 8.0f));
    }

    static constexpr const TriggerEffect appleVibration(const float amplitudes[10],
                                                        float frequency) {
      uint8_t levels[10];
      for (unsigned i = 0; i < 10; ++i) levels[i] = normalize(amplitudes[i], 8.0f);
      return multiplePositionVibration(normalize(frequency, 255.0f), levels);
    }

    // reWASD traffic captures, including its intentionally non-standard values.
    static constexpr const TriggerEffect rewasdFullPress() {
      return simpleWeapon(0x90, 0xA0, 0xFF);
    }
    static constexpr const TriggerEffect rewasdSoftPress() {
      return simpleWeapon(0x70, 0xA0, 0xFF);
    }
    static constexpr const TriggerEffect rewasdMediumPress() {
      return simpleWeapon(0x45, 0xA0, 0xFF);
    }
    static constexpr const TriggerEffect rewasdHardPress() {
      return simpleWeapon(0x20, 0xA0, 0xFF);
    }
    static constexpr const TriggerEffect rewasdPulse() { return simpleWeapon(0x00, 0x00, 0x00); }

    static constexpr const TriggerEffect rewasdChoppy() {
      TriggerEffect effect(Mode::Feedback);
      effect.parameters[0] = 0x02;
      effect.parameters[1] = 0x27;
      effect.parameters[2] = 0x18;
      effect.parameters[3] = 0x00;
      effect.parameters[4] = 0x00;
      effect.parameters[5] = 0x26;
      return effect;
    }

    static constexpr const TriggerEffect rewasdSoftRigidity() { return simpleFeedback(0x00, 0x00); }
    static constexpr const TriggerEffect rewasdMediumRigidity() {
      return simpleFeedback(0x00, 0x64);
    }
    static constexpr const TriggerEffect rewasdMaxRigidity() { return simpleFeedback(0x00, 0xDC); }
    static constexpr const TriggerEffect rewasdHalfPress() { return simpleFeedback(0x55, 0x64); }

    static constexpr const TriggerEffect rewasdRifle(uint8_t frequency = 10) {
      if (frequency < 2 || frequency > 20) return TriggerEffect{};
      TriggerEffect effect(Mode::Vibration);
      effect.parameters[1] = 0x03;
      effect.parameters[5] = 0x3F;
      effect.parameters[8] = frequency;
      return effect;
    }

    static constexpr const TriggerEffect rewasdVibration(uint8_t strength  = 220,
                                                         uint8_t frequency = 30) {
      if (strength < 1 || frequency < 1) return TriggerEffect{};
      TriggerEffect effect(Mode::Vibration);
      effect.parameters[1] = 0x03;
      effect.parameters[5] = strength;
      effect.parameters[8] = frequency;
      return effect;
    }
  };
  static_assert(sizeof(TriggerEffect) == 11, "trigger effect must stay 11 bytes");

private:
  using DSUINT8  = internal::DSUINT8;
  using DSUINT32 = internal::DSUINT32;

protected:
  DSUINT8 validFlag0;          // +0x00  ValidFlag0 bits
  DSUINT8 validFlag1;          // +0x01  ValidFlag1 bits
  DSUINT8 motorRight;          // +0x02  DS4-compatible rumble
  DSUINT8 motorLeft;           // +0x03
  DSUINT8 volumeHeadphones;    // +0x04  0x00..0x7f
  DSUINT8 volumeSpeaker;       // +0x05  0x00..0xff
  DSUINT8 micVolume;           // +0x06  0x00..0x40 (claimed by MicVolume)
  DSUINT8 audioControl;        // +0x07  AudioControl; claimed by AudioRouting
  DSUINT8 muteButtonLed;       // +0x08
  DSUINT8 powerSaveControl;    // +0x09  PowerSave bits
  TriggerEffect rightTrigger;  // +0x0a  11-byte effect block
  TriggerEffect leftTrigger;   // +0x15  11-byte effect block
  DSUINT32 hostTimestamp;      // +0x20  mirrored input sensor timestamp
  DSUINT8 motorPowerLevel;     // +0x24  trigger:rumble reduction nibbles
  DSUINT8 audioControl2;       // +0x25  bits 0..2: SP preamp gain; other bits unknown
  DSUINT8 validFlag2;          // +0x26  ValidFlag2 bits
  DSUINT8 hapticLowPassFilter; // +0x27  hardware haptic LPF enable
  DSUINT8 reserved40;          // +0x28
  DSUINT8 lightbarSetup;       // +0x29  validFlag2 bit 1; 0x02 fades out
  DSUINT8 playerLedBrightness; // +0x2a  0=high, 1=medium, 2=low
  DSUINT8 playerLeds;          // +0x2b  PlayerLeds bits
  DSUINT8 lightbarRed;         // +0x2c
  DSUINT8 lightbarGreen;       // +0x2d
  DSUINT8 lightbarBlue;        // +0x2e

public:
  static constexpr uint8_t micMaxVolume = 0x40;

  constexpr uint8_t headphoneVolume() const { return volumeHeadphones; }
  constexpr uint8_t speakerVolume() const { return volumeSpeaker; }

private:
  // Which fields this report owns: the kernel's (and DS4Windows', and
  // dualsense-tester's) validFlag0 / validFlag1 / validFlag2. A set bit
  // means "adopt my bytes for this feature"; a clear bit means "leave that
  // feature as it is" — a claim, not a change notification.
  //
  // Example: RightTriggerMotorEnable | LeftTriggerMotorEnable claims only the two
  // trigger blocks,
  // leaving volumes, LEDs and rumble untouched.
  enum class ValidFlag0 : uint8_t {
    CompatibleVibration     = 1u << 0, // +0x00 bit 0; main motors also need HapticsSelect
    HapticsSelect           = 1u << 1, //       bit 1; paired with CompatibleVibration
    RightTriggerMotorEnable = 1u << 2, //       bit 2
    LeftTriggerMotorEnable  = 1u << 3, //       bit 3
    HeadphoneVolumeEnable   = 1u << 4, //       bit 4
    SpeakerVolumeEnable     = 1u << 5, //       bit 5
    MicVolumeEnable         = 1u << 6, //       bit 6
    AudioControlEnable      = 1u << 7, //       bit 7
  };

  enum class ValidFlag1 : uint8_t {
    MicMuteLedControlEnable      = 1u << 0, // +0x01 bit 0
    PowerSaveControlEnable       = 1u << 1, //       bit 1  enables powerSaveControl
    LightbarControlEnable        = 1u << 2, //       bit 2
    ReleaseLEDs                  = 1u << 3, //       bit 3
    PlayerIndicatorControlEnable = 1u << 4, //       bit 4
    HapticLowPassFilterEnable    = 1u << 5, //       bit 5
    VibrationAttenuationEnable   = 1u << 6, //       bit 6
    AudioControl2Enable          = 1u << 7, //       bit 7
  };

  // +0x26. Bit 0 applies playerLedBrightness; bit 1 enables the lightbar
  // fade control. Bits 3-7 are reserved.
  enum class ValidFlag2 : uint8_t {
    PlayerLedBrightnessControlEnable = 1u << 0, // bit 0
    LightbarSetupControlEnable       = 1u << 1, // bit 1: enables the lightbarSetup fade control
    CompatibleVibration2             = 1u << 2, // bit 2
  };

  // +0x09. Power-save controls. hid-playstation.c documents microphone mute
  // and audio mute; the other names come from the community schemas. Some
  // controllers stop reporting motion while MotionPowerSave is set.
  enum PowerSave : uint8_t {
    TouchPowerSave  = 1u << 0, // bit 0
    MotionPowerSave = 1u << 1, // bit 1
    HapticPowerSave = 1u << 2, // bit 2
    AudioPowerSave  = 1u << 3, // bit 3
    MicMute         = 1u << 4, // bit 4
    SpeakerMute     = 1u << 5, // bit 5
    HeadphoneMute   = 1u << 6, // bit 6
    HapticMute      = 1u << 7, // bit 7
  };

  // +0x2b: the five player LEDs, the same pattern the charging sweep animates.
  // Bits 5-7 are reserved.
  enum class PlayerLeds : uint8_t {
    Led1    = 1u << 0, // bit 0
    Led2    = 1u << 1, // bit 1
    Led3    = 1u << 2, // bit 2
    Led4    = 1u << 3, // bit 3
    Led5    = 1u << 4, // bit 4
    Instant = 1u << 5, // bit 5: suppress the slow fade-in
  };

public:
  // Shared brightness of the five player indicator LEDs. Firmware encoding is
  // inverted: lower values are brighter. This does not affect the RGB lightbar.
  enum class PlayerLedBrightness : uint8_t {
    High   = 0,
    Medium = 1,
    Low    = 2,
  };

  enum class LightbarSetup : uint8_t {
    LightOn  = 1u << 0,
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
    Speaker    = static_cast<uint8_t>(AudioOutputPath::X_X_R) << 4,
  };

private:
  static constexpr uint8_t bit(ValidFlag0 f) { return static_cast<uint8_t>(f); }
  static constexpr uint8_t bit(ValidFlag1 f) { return static_cast<uint8_t>(f); }
  static constexpr uint8_t bit(ValidFlag2 f) { return static_cast<uint8_t>(f); }
  constexpr void claim(ValidFlag0 f) { validFlag0 = static_cast<uint8_t>(validFlag0 | bit(f)); }
  constexpr void claim(ValidFlag1 f) { validFlag1 = static_cast<uint8_t>(validFlag1 | bit(f)); }
  constexpr void claim(ValidFlag2 f) { validFlag2 = static_cast<uint8_t>(validFlag2 | bit(f)); }

public:
  constexpr void setMotorRight(uint8_t v) {
    claim(ValidFlag0::CompatibleVibration);
    claim(ValidFlag0::HapticsSelect);
    motorRight = v;
  }

  constexpr void setMotorLeft(uint8_t v) {
    claim(ValidFlag0::CompatibleVibration);
    claim(ValidFlag0::HapticsSelect);
    motorLeft = v;
  }

  constexpr void setHeadphoneVolume(uint8_t v) {
    claim(ValidFlag0::HeadphoneVolumeEnable);
    volumeHeadphones = v;
  }

  constexpr void setSpeakerVolume(uint8_t v) {
    claim(ValidFlag0::SpeakerVolumeEnable);
    volumeSpeaker = v;
  }

  constexpr void setMicVolume(uint8_t v) {
    claim(ValidFlag0::MicVolumeEnable);
    micVolume = v;
  }

  constexpr void setAudioRouting(uint8_t v) {
    claim(ValidFlag0::AudioControlEnable);
    audioControl = v;
  }

  constexpr void setMuteButtonLed(uint8_t v) {
    claim(ValidFlag1::MicMuteLedControlEnable);
    muteButtonLed = v;
  }

  constexpr void setPowerSaveControl(uint8_t v) {
    claim(ValidFlag1::PowerSaveControlEnable);
    powerSaveControl = v;
  }

  constexpr void setMotorPowerLevel(uint8_t v) {
    claim(ValidFlag1::VibrationAttenuationEnable);
    motorPowerLevel = v;
  }

  constexpr void setHapticLowPassFilter(bool on) {
    claim(ValidFlag1::HapticLowPassFilterEnable);
    hapticLowPassFilter = on ? 1 : 0;
  }

  constexpr void setHostTimestamp(uint32_t v) { hostTimestamp = v; }

  // Sets the low three bits of audioControl2. 0x2 is +6 dB; the other gain
  // codes are not documented. Upper bits of the byte are preserved.
  constexpr void setSpeakerPreampGain(uint8_t gainCode) {
    claim(ValidFlag1::AudioControl2Enable);
    audioControl2 = static_cast<uint8_t>((audioControl2 & 0xF8) | (gainCode & 0x07));
  }

  constexpr void releaseLEDs() { claim(ValidFlag1::ReleaseLEDs); }

  constexpr void setLightbarSetup(LightbarSetup v) {
    claim(ValidFlag2::LightbarSetupControlEnable);
    lightbarSetup = static_cast<uint8_t>(v);
  }

  constexpr void setPlayerLedBrightness(PlayerLedBrightness level) {
    claim(ValidFlag2::PlayerLedBrightnessControlEnable);
    playerLedBrightness = static_cast<uint8_t>(level);
  }

  constexpr void setPlayerLeds(bool led1, bool led2, bool led3, bool led4, bool led5,
                               bool instant) {
    claim(ValidFlag1::PlayerIndicatorControlEnable);
    uint8_t mask = 0;
    if (led1) mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(PlayerLeds::Led1));
    if (led2) mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(PlayerLeds::Led2));
    if (led3) mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(PlayerLeds::Led3));
    if (led4) mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(PlayerLeds::Led4));
    if (led5) mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(PlayerLeds::Led5));
    if (instant) mask = static_cast<uint8_t>(mask | static_cast<uint8_t>(PlayerLeds::Instant));
    playerLeds = mask;
  }

  constexpr void setLightbar(uint8_t red, uint8_t green, uint8_t blue) {
    claim(ValidFlag1::LightbarControlEnable);
    lightbarRed   = red;
    lightbarGreen = green;
    lightbarBlue  = blue;
  }

  constexpr void setRightTriggerEffect(const TriggerEffect &effect) {
    claim(ValidFlag0::RightTriggerMotorEnable);
    rightTrigger = effect;
  }

  constexpr void setLeftTriggerEffect(const TriggerEffect &effect) {
    claim(ValidFlag0::LeftTriggerMotorEnable);
    leftTrigger = effect;
  }

  constexpr void setAudioRoute(AudioControl routing) {
    claim(ValidFlag0::AudioControlEnable);
    audioControl = static_cast<uint8_t>(routing);
  }

  constexpr void setAudio(AudioControl routing) {
    // Keep the main-motor claims so a 0x36 snapshot does not stop a 0x31
    // rumble on the next packet. Trigger claims stay cleared: this stream
    // writes zeros into those blocks.
    constexpr uint8_t motorMask =
        static_cast<uint8_t>(bit(ValidFlag0::CompatibleVibration) | bit(ValidFlag0::HapticsSelect));
    validFlag0 = static_cast<uint8_t>(
        (validFlag0 & motorMask) | bit(ValidFlag0::HeadphoneVolumeEnable) |
        bit(ValidFlag0::SpeakerVolumeEnable) | bit(ValidFlag0::MicVolumeEnable) |
        bit(ValidFlag0::AudioControlEnable));
    validFlag1       = static_cast<uint8_t>(validFlag1 | bit(ValidFlag1::AudioControl2Enable));
    volumeHeadphones = 0x7F;
    volumeSpeaker    = 0x64;
    audioControl     = static_cast<uint8_t>(routing);
    audioControl2    = 0x0A;
  }

  constexpr void setMicMuted(bool muted) {
    claim(ValidFlag1::PowerSaveControlEnable);
    constexpr uint8_t mask = static_cast<uint8_t>(PowerSave::MicMute);
    powerSaveControl       = muted ? static_cast<uint8_t>(powerSaveControl | mask)
                                   : static_cast<uint8_t>(powerSaveControl & ~mask);
  }

  // 0 mutes capture; 1..micMaxVolume enables it at that volume.
  constexpr void setMic(uint8_t volume) {
    setMicVolume(volume > micMaxVolume ? micMaxVolume : volume);
    setMicMuted(volume == 0);
  }

  constexpr void clearLightbar() {
    validFlag1    = static_cast<uint8_t>(validFlag1 & ~bit(ValidFlag1::LightbarControlEnable));
    validFlag2    = static_cast<uint8_t>(validFlag2 & ~bit(ValidFlag2::LightbarSetupControlEnable));
    lightbarSetup = 0;
    lightbarRed   = 0;
    lightbarGreen = 0;
    lightbarBlue  = 0;
  }

  constexpr void clearClaims() {
    validFlag0 = 0;
    validFlag1 = 0;
    validFlag2 = 0;
  }

  constexpr void clearOneShotClaims() {
    validFlag0 = static_cast<uint8_t>(validFlag0 & ~(bit(ValidFlag0::RightTriggerMotorEnable) |
                                                     bit(ValidFlag0::LeftTriggerMotorEnable)));
    validFlag1 = static_cast<uint8_t>(
        validFlag1 &
        ~(bit(ValidFlag1::MicMuteLedControlEnable) | bit(ValidFlag1::LightbarControlEnable) |
          bit(ValidFlag1::ReleaseLEDs) | bit(ValidFlag1::PlayerIndicatorControlEnable)));
    validFlag2 = 0;
  }

  constexpr void claimMotors() {
    claim(ValidFlag0::CompatibleVibration);
    claim(ValidFlag0::HapticsSelect);
  }

  // Only the motors are refreshed by 0x36; the controller stops rumble if
  // their claim disappears from the audio stream.
  constexpr OutputBody motorSnapshot() const {
    OutputBody out = *this;
    out.clearClaims();
    out.claimMotors();
    return out;
  }

  constexpr OutputBody audioSnapshot() const {
    OutputBody out = *this;
    out.clearOneShotClaims();
    return out;
  }

  constexpr OutputBody() {}
  // Claims only what the audio path writes: the volumes and the routing.
  constexpr explicit OutputBody(AudioControl routing) : OutputBody() {
    validFlag0 = static_cast<uint8_t>(
        bit(ValidFlag0::HeadphoneVolumeEnable) | bit(ValidFlag0::SpeakerVolumeEnable) |
        bit(ValidFlag0::MicVolumeEnable) | bit(ValidFlag0::AudioControlEnable));
    validFlag1       = bit(ValidFlag1::AudioControl2Enable);
    volumeHeadphones = 0x7F;
    volumeSpeaker    = 0x64;
    audioControl     = static_cast<uint8_t>(routing);
    audioControl2    = 0x0A;
  }
};
static_assert(sizeof(OutputBody) == 47, "output body must stay 47 bytes");

// Saturating conversions used by the Swift schema when mapping normalized
// [0, 1] values onto the byte/zone/level wire fields.
struct DSScale {
  static constexpr float clamp01(float value) {
    if (!(value > -1.0e30f && value < 1.0e30f)) return 0;
    return value < 0 ? 0 : (value > 1 ? 1 : value);
  }

  static constexpr uint8_t byte(float value) {
    float v    = clamp01(value);
    int scaled = static_cast<int>(v * 255.0f + 0.5f);
    return static_cast<uint8_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
  }

  static constexpr uint8_t rawByte(float value) {
    if (!(value > -1.0e30f && value < 1.0e30f)) return 0;
    int scaled = static_cast<int>(value + (value >= 0 ? 0.5f : -0.5f));
    return static_cast<uint8_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
  }

  static constexpr int zone(float value) {
    float v    = clamp01(value) * 9.0f;
    int scaled = static_cast<int>(v + 0.5f);
    return scaled < 0 ? 0 : (scaled > 9 ? 9 : scaled);
  }

  static constexpr uint8_t level(float value) {
    float v    = clamp01(value) * 8.0f;
    int scaled = static_cast<int>(v + 0.5f);
    return static_cast<uint8_t>(scaled < 0 ? 0 : (scaled > 8 ? 8 : scaled));
  }
};

// ===========================================================================
// Output 0x31 (Bluetooth, 78 bytes, CRC at the end)
// ===========================================================================

// Wire report types. The internal namespace owns storage and probes; these
// public wrappers expose construction, accessors and framing helpers.
struct OutputReport {
private:
  using DSUINT8  = internal::DSUINT8;
  using DSUINT32 = internal::DSUINT32;
  using ReportId = internal::ReportId;

protected:
  ReportId reportIdValue; // 0x00  ReportId::StateBluetooth
  DSUINT8 seqTag;         // 0x01  high nibble = counter 0..15, low nibble = 0
  DSUINT8 tagValue;       // 0x02  must be 0x10
  OutputBody bodyData;    // 0x03..0x31
  DSUINT8 reserved4[24];  // 0x32..0x49
  DSUINT32 crc;           // 0x4a  CRC-32, seed 0xA2, little-endian

private:
  static constexpr ReportId reportId = ReportId::StateBluetooth;
  static constexpr uint8_t tag       = 0x10;

public:
  // 0x31 sequence tagValue: high nibble counts 0..15, low nibble stays zero.
  struct Sequence {
    constexpr Sequence() : value(0), counter(0) {}
    constexpr uint8_t next() {
      uint8_t next_tag = static_cast<uint8_t>((value & mask) << shift);
      value            = static_cast<uint8_t>((value + 1) & mask);
      return next_tag;
    }

    constexpr uint8_t nextCounter() { return counter++; }

  private:
    uint8_t value;
    uint8_t counter;
    static constexpr uint8_t mask  = 0x0F;
    static constexpr uint8_t shift = 4;
  };

  constexpr OutputReport() : reportIdValue(reportId), tagValue(tag) {}
  constexpr explicit OutputReport(OutputReport::Sequence &sequence) : OutputReport() {
    seqTag = sequence.next();
  }

  constexpr const OutputBody &body() const { return bodyData; }
  constexpr OutputBody &body() { return bodyData; }
  constexpr void sign() {
    constexpr uint8_t crcSeed = 0xA2;
    crc                       = internal::crc_of(*this, crcSeed);
  }
};
static_assert(sizeof(OutputReport) == 78, "0x31 must stay 78 bytes");

// Output 0x02 (USB, 63 bytes). No CRC — the kernel signs only the Bluetooth
// transport — and the same 47-byte body; sizes from hid-playstation.c:
// DS_OUTPUT_REPORT_USB_SIZE 63, DS_OUTPUT_REPORT_BT_SIZE 78.
struct UsbOutputReport {
private:
  using DSUINT8  = internal::DSUINT8;
  using ReportId = internal::ReportId;

protected:
  ReportId reportIdValue; // 0x00  ReportId::OutputUsb
  OutputBody bodyData;    // 0x01..0x2f
  DSUINT8 reserved[15];   // 0x30..0x3e

private:
  static constexpr ReportId reportId = ReportId::OutputUsb;

public:
  constexpr UsbOutputReport() : reportIdValue(reportId) {}

  constexpr const OutputBody &body() const { return bodyData; }
  constexpr OutputBody &body() { return bodyData; }
};
static_assert(sizeof(UsbOutputReport) == 63, "0x02 must stay 63 bytes");

// ===========================================================================
// Input: the 63-byte body shared by 0x31 (Bluetooth) and 0x01 (USB)
// ===========================================================================

// Input body shared by USB 0x01 and Bluetooth 0x31. With microphone duplex
// enabled, 0x31 frames carrying Opus TOC 0xD4 are audio, not gamepad data.
struct InputBody {
private:
  using DSUINT8  = internal::DSUINT8;
  using DSUINT32 = internal::DSUINT32;
  using DSINT16  = internal::DSINT16;

public:
  // One 4-byte touch contact on the wire. The high nibble of `xyHigh` is
  // y-low; the low nibble is x-high. Bit 7 of `contact` means "not touching".
  struct TouchPoint {
  private:
    DSUINT8 contact;
    DSUINT8 xLow;
    DSUINT8 xyHigh;
    DSUINT8 yHigh;

  public:
    constexpr bool active() const { return (contact & 0x80) == 0; }
    constexpr unsigned id() const { return contact & 0x7F; }

    constexpr unsigned x() const { return static_cast<unsigned>(xLow | ((xyHigh & 0x0F) << 8)); }

    constexpr unsigned y() const { return static_cast<unsigned>((xyHigh >> 4) | (yHigh << 4)); }
  };
  static_assert(sizeof(TouchPoint) == 4, "touch point must stay 4 bytes");

  struct TouchData {
  protected:
    TouchPoint fingers[2];
    DSUINT8 timestamp;

  public:
    constexpr const TouchPoint &getFinger(unsigned i) const { return fingers[i == 0 ? 0 : 1]; }
  };
  static_assert(sizeof(TouchData) == 9, "touch data must stay 9 bytes");

  struct TriggerFeedback {
  protected:
    uint8_t value;

  public:
    constexpr unsigned stopLocation() const { return value & 0x0F; }
    constexpr unsigned status() const { return value >> 4; }
  };
  static_assert(sizeof(TriggerFeedback) == 1, "trigger feedback must stay 1 byte");

protected:
  DSUINT8 lx;                   // +0x00  left stick X, 0..255 (128 = centre)
  DSUINT8 ly;                   // +0x01  left stick Y, 0 = up
  DSUINT8 rx;                   // +0x02  right stick X
  DSUINT8 ry;                   // +0x03  right stick Y
  DSUINT8 l2;                   // +0x04  L2 analogue
  DSUINT8 r2;                   // +0x05  R2 analogue
  DSUINT8 seq;                  // +0x06  report sequence number
  DSUINT32 buttonsData;         // +0x07  Buttons bits (d-pad in bits 0-3)
  DSUINT8 incremental;          // +0x0b  increments every report, wraps at 256
  DSUINT8 reserved14[3];        // +0x0c  constant (0x84, ?, 0x85) except that the
                                //        middle byte ticks up by one every few
                                //        minutes — a slow counter
  DSINT16 gyroX;                // +0x0f  angular velocity, little-endian
  DSINT16 gyroY;                // +0x11
  DSINT16 gyroZ;                // +0x13
  DSINT16 accelX;               // +0x15  acceleration, ~8192 counts per g
  DSINT16 accelY;               // +0x17
  DSINT16 accelZ;               // +0x19
  DSUINT32 sensorTime;          // +0x1b  timestamp, 0.33 us units
  DSUINT8 temperatureC;         // +0x1f  controller temperature, degrees C
  TouchData touchData;          // +0x20  two contacts + timestamp
  TriggerFeedback triggerRight; // +0x29  stop location + status
  TriggerFeedback triggerLeft;  // +0x2a
  DSUINT32 hostTimestamp;       // +0x2b  echoed host timestamp
  DSUINT8 triggerEffect;        // +0x2f  low nibble right, high nibble left
  DSUINT32 deviceTimestamp;     // +0x30  device clock
  DSUINT8 status0;              // +0x34  low nibble battery, high power state
  DSUINT8 status1;              // +0x35  Status1 bits
  DSUINT8 status2;              // +0x36  Status2 bits
  DSUINT8 aesCmac[8];           // +0x37  high entropy in every report, like a
                                //        per-report MAC/nonce; not verified
                                //        against the pairing handshake

  static constexpr uint32_t dpadMask = 0x0F;

public:
  struct Stick {
    int x;
    int y;
  };

  struct Triggers {
    int left;
    int right;
  };

  static constexpr int touchpadWidth  = 1920;
  static constexpr int touchpadHeight = 1080;

  Stick leftStick() const { return {lx, ly}; }
  Stick rightStick() const { return {rx, ry}; }
  Triggers triggers() const { return {l2, r2}; }
  int sequence() const { return seq; }

  // Normalized full-scale angular rate: -1..1.
  struct Gyro {
    float x, y, z;

    constexpr Gyro(int raw_x, int raw_y, int raw_z)
        : x(static_cast<float>(raw_x) / countsPerFullScale),
          y(static_cast<float>(raw_y) / countsPerFullScale),
          z(static_cast<float>(raw_z) / countsPerFullScale) {}

  private:
    static constexpr float countsPerFullScale = 32768.f;
  };

  Gyro gyro() const { return {gyroX, gyroY, gyroZ}; }

  // Acceleration in g: ~8192 raw counts per g.
  struct Accelerometer {
    float x, y, z;

    constexpr Accelerometer(int raw_x, int raw_y, int raw_z)
        : x(static_cast<float>(raw_x) / countsPerG), y(static_cast<float>(raw_y) / countsPerG),
          z(static_cast<float>(raw_z) / countsPerG) {}
    float len() const { return sqrtf(x * x + y * y + z * z); }

  private:
    static constexpr float countsPerG = 8192.f;
  };

  Accelerometer accelerometer() const { return {accelX, accelY, accelZ}; }
  uint8_t temperature() const { return temperatureC; }

  // The d-pad is a 4-bit *hat value*, not four bits. The enum turns it into a
  // name; diagonals are the odd values.
  enum class Dpad : uint8_t {
    Up        = 0,
    UpRight   = 1,
    Right     = 2,
    DownRight = 3,
    Down      = 4,
    DownLeft  = 5,
    Left      = 6,
    UpLeft    = 7,
    None      = 8,
  };

  Dpad dpad() const { return static_cast<Dpad>(buttonsData & dpadMask); }

  // Four button bytes merged into one little-endian 32-bit field: bits 0..7
  // are the byte at +0x07, 8..15 at +0x08, and so on. Bit positions:
  //   +0x07  bits 0-3 d-pad hat (Dpad), 4 square, 5 cross, 6 circle, 7 triangle
  //   +0x08  bits 0 l1, 1 r1, 2 l2, 3 r2, 4 create, 5 options, 6 l3, 7 r3
  //   +0x09  bits 0 ps, 1 touchpad, 2 mute, 3 reserved, 4 fn1, 5 fn2,
  //          6 left_paddle, 7 right_paddle
  //   +0x0a  eight bits the kernel does not read; maybe L5/R5
  enum class Button : uint32_t {
    // +0x07
    ButtonSquare    = 1u << 4,
    ButtonCross     = 1u << 5,
    ButtonCircle    = 1u << 6,
    ButtonTriangle  = 1u << 7,
    // +0x08
    L1              = 1u << 8,
    R1              = 1u << 9,
    L2              = 1u << 10,
    R2              = 1u << 11,
    Create          = 1u << 12,
    Options         = 1u << 13,
    L3              = 1u << 14,
    R3              = 1u << 15,
    // +0x09
    PsHome          = 1u << 16,
    Touchpad        = 1u << 17,
    MicMute         = 1u << 18,
    // bit 19 reserved
    EdgeFn1         = 1u << 20, // Edge only
    EdgeFn2         = 1u << 21, // Edge only
    EdgeLeftPaddle  = 1u << 22, // Edge only
    EdgeRightPaddle = 1u << 23, // Edge only
    // +0x0a: unread by the kernel; maybe L5/R5
  };

  struct Buttons {
    friend struct InputBody;

    constexpr Buttons() : buttons(0) {}
    constexpr bool operator!=(const Buttons &o) const { return buttons != o.buttons; }
    constexpr bool isOn(Button b) const { return (buttons & static_cast<uint32_t>(b)) != 0; }

    const Buttons pressed(const Buttons &prev) const { return Buttons(buttons & ~prev.buttons); }

  private:
    constexpr Buttons(uint32_t v) : buttons(v) {}

    uint32_t buttons;
  };

  constexpr const Buttons buttons() const { return Buttons(buttonsData & ~dpadMask); }

  // status0's low nibble is a coarse 0..10 battery level (8 -> 80%); its
  // high nibble is PowerState. The controller can report Complete before the
  // level reaches 10, especially on an aged pack.
  constexpr int battery() const {
    int percent = (status0 & 0x0F) * 10;
    return percent > 100 ? 100 : percent;
  }

  enum class PowerState : uint8_t {
    Discharging         = 0x00,
    Charging            = 0x01,
    Complete            = 0x02,
    AbnormalVoltage     = 0x0A,
    AbnormalTemperature = 0x0B,
    ChargingError       = 0x0F,
  };

  constexpr PowerState powerState() const { return static_cast<PowerState>(status0 >> 4); }

  // +0x35. PluggedUsbData/Power are the two bits observed during USB/charger
  // testing; the kernel only documents the headset/mic/mute bits.
  enum class Status1 : uint8_t {
    HeadphoneDetect = 1u << 0, // bit 0
    MicDetect       = 1u << 1, // bit 1
    MicMute         = 1u << 2, // bit 2
    PluggedUsbData  = 1u << 3, // bit 3
    PluggedUsbPower = 1u << 4, // bit 4
  };

  // +0x36. duaLib names these two; the remaining bits are still unknown.
  enum class Status2 : uint8_t {
    PluggedExternalMic  = 1u << 0, // bit 0
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

  TouchPoint touch(unsigned index) const { return touchData.getFinger(index); }
  unsigned triggerStopLocationLeft() const { return triggerLeft.stopLocation(); }
  unsigned triggerStatusLeft() const { return triggerLeft.status(); }
  unsigned triggerStopLocationRight() const { return triggerRight.stopLocation(); }
  unsigned triggerStatusRight() const { return triggerRight.status(); }
  unsigned triggerEffectLeft() const { return triggerEffect >> 4; }
  unsigned triggerEffectRight() const { return triggerEffect & 0x0F; }
  uint32_t hostTime() const { return hostTimestamp; }
  uint32_t deviceTime() const { return deviceTimestamp; }
};
static_assert(sizeof(InputBody) == 63, "input body must stay 63 bytes");

// ===========================================================================
// Input 0x31 (Bluetooth, 78 bytes) and 0x01 (USB, 64 bytes)
//
// The Bluetooth report is the USB one plus a sequence tagValue in front and a
// nine-byte tail plus a CRC behind, so every shared field's USB offset is its
// Bluetooth offset minus one. The tail's first byte is not constant (30
// distinct values in a 150 s capture), the other eight were; what it carries is
// unknown. Sizes come from hid-playstation.c (DS_INPUT_REPORT_USB_SIZE 64,
// DS_INPUT_REPORT_BT_SIZE 78); the CRC covers the report with seed 0xA1.
// ===========================================================================

struct InputReport {
private:
  using DSUINT8  = internal::DSUINT8;
  using DSUINT32 = internal::DSUINT32;
  using ReportId = internal::ReportId;

protected:
  ReportId reportIdValue; // 0x00  ReportId::StateBluetooth
  DSUINT8 seqTag;         // 0x01  high nibble = sequence tagValue, low nibble = 0
  InputBody bodyData;     // 0x02..0x40
  DSUINT8 btReserved[9];  // 0x41..0x49  the first byte is not constant (30
                          //             distinct values in a 150 s capture), the
                          //             other eight were; unknown
  DSUINT32 crc;           // 0x4a  CRC-32 over the report, seed 0xA1

private:
  static constexpr ReportId reportId = ReportId::StateBluetooth;
  static constexpr uint8_t crcSeed   = 0xA1;
  constexpr bool isValid() const { return crc == internal::crc_of(*this, crcSeed); }

public:
  constexpr InputReport() : reportIdValue(reportId) { bodyData = InputBody{}; }

  constexpr const InputBody &body() const { return bodyData; }
  constexpr InputBody &body() { return bodyData; }
  constexpr void sign() { crc = internal::crc_of(*this, crcSeed); }

  static const InputReport *asFrame(const uint8_t *buf, size_t len) {
    if (!buf || len < sizeof(InputReport) || buf[0] != static_cast<uint8_t>(reportId))
      return nullptr;
    const auto *r = reinterpret_cast<const InputReport *>(buf);
    return r->isValid() ? r : nullptr;
  }
};
static_assert(sizeof(InputReport) == 78, "input 0x31 must stay 78 bytes");

// USB 0x01 input report: one report-id byte followed by the same 63-byte body.
struct UsbInputReport {
private:
  using DSUINT8                      = internal::DSUINT8;
  using ReportId                     = internal::ReportId;
  static constexpr ReportId reportId = ReportId::InputUsb;

protected:
  ReportId reportIdValue; // 0x00  ReportId::InputUsb
  InputBody bodyData;     // 0x01..0x3f

public:
  constexpr UsbInputReport() : reportIdValue(reportId) { bodyData = InputBody{}; }

  constexpr const InputBody &body() const { return bodyData; }

  static const UsbInputReport *asFrame(const uint8_t *buf, size_t len) {
    if (!buf || len < sizeof(UsbInputReport) || buf[0] != static_cast<uint8_t>(reportId))
      return nullptr;
    return reinterpret_cast<const UsbInputReport *>(buf);
  }
};
static_assert(sizeof(UsbInputReport) == 64, "input 0x01 must stay 64 bytes");

// ===========================================================================
// Audio 0x36 (Bluetooth, 398 bytes)
// ===========================================================================
//
// Bluetooth speaker/haptics stream: config, a zeroed state block, 6 kHz
// haptics PCM and a 200-byte Opus speaker frame. The microphone returns as 0xD4
// frames in 0x31; output state is sent separately in 0x31 reports.

struct HapticsReport;

struct AudioReport {
  friend struct internal::WireProbe;
  friend struct HapticsReport;

public:
  AudioReport(const AudioReport &)            = delete;
  AudioReport &operator=(const AudioReport &) = delete;

private:
  using DSUINT8  = internal::DSUINT8;
  using DSINT8   = internal::DSINT8;
  using DSUINT32 = internal::DSUINT32;
  using DSINT16  = internal::DSINT16;
  using ReportId = internal::ReportId;

public:
  enum class SubPacketId : uint8_t {
    State   = 0x10,
    Config  = 0x11,
    Data    = 0x12,
    Speaker = 0x13,
    Jack    = 0x16,
  };

  struct PacketHeader {
  private:
    DSUINT8 pidValue;
    DSUINT8 lengthValue;

  public:
    constexpr PacketHeader() : pidValue(0), lengthValue(0) {}
    constexpr explicit PacketHeader(SubPacketId id, uint8_t length)
        : pidValue(static_cast<uint8_t>(id) | packetSizedBit), lengthValue(length) {}
    PacketHeader(const PacketHeader &)            = delete;
    PacketHeader &operator=(const PacketHeader &) = delete;

    constexpr uint8_t pid() const { return pidValue; }
    constexpr uint8_t length() const { return lengthValue; }
    constexpr void set(SubPacketId id, uint8_t length) {
      pidValue    = static_cast<uint8_t>(id) | packetSizedBit;
      lengthValue = length;
    }
  };
  static_assert(sizeof(PacketHeader) == 2, "packet header must stay 2 bytes");

  struct PcmPacket {
  private:
    PacketHeader headerValue;
    DSINT8 samplesValue[64];

  public:
    constexpr PcmPacket() : samplesValue() {
      for (auto &sample : samplesValue) sample = 0;
    }
    PcmPacket(const PcmPacket &)            = delete;
    PcmPacket &operator=(const PcmPacket &) = delete;

    constexpr const PacketHeader &header() const { return headerValue; }
    constexpr void setHeader(SubPacketId id, uint8_t length) { headerValue.set(id, length); }

    template <typename Fill>
    constexpr void fillSamples(Fill fill) {
      for (size_t i = 0; i < 64; ++i) samplesValue[i] = fill(i);
    }

    constexpr void setSamples(const int8_t *data, size_t len) {
      size_t n = len < 64 ? len : 64;
      for (size_t i = 0; i < n; ++i) samplesValue[i] = data[i];
      for (size_t i = n; i < 64; ++i) samplesValue[i] = 0;
    }

    constexpr int8_t sample(size_t i) const { return samplesValue[i]; }
  };
  static_assert(sizeof(PcmPacket) == 66, "PCM sub-packet must stay 66 bytes");

  // +0x004. Bit 0 is the microphone capture request: 0xFE keeps it shut, 0xFF
  // opens it and the input stream then also carries 0xD4 frames.
  enum class ConfigMask : uint8_t {
    MicrophoneCapture = 0x01,
    SpeakerOnly       = 0xFE,
    SpeakerAndMic     = 0xFF,
  };

  struct AudioConfig {
    ConfigMask mask;
    DSUINT8 hapticLengths[5];
    DSUINT8 counter;

    constexpr AudioConfig() : mask(), hapticLengths(), counter() {}
    AudioConfig(const AudioConfig &)            = delete;
    AudioConfig &operator=(const AudioConfig &) = delete;
  };
  static_assert(sizeof(AudioConfig) == 7, "audio config must stay 7 bytes");

private:
  struct State {
    friend struct AudioReport;

  protected:
    OutputBody body;     // +0x00..0x2e
    DSUINT8 padding[16]; // +0x2f..0x3e  the rest of the 63-byte block
  };
  static_assert(sizeof(State) == 63, "State must stay 63 bytes");

protected:
  ReportId reportIdValue;    // 0x000  ReportId::Audio
  DSUINT8 seqTag;            // 0x001  high nibble = counter, low nibble = 0
  PacketHeader configHeader; // 0x002  0x11 | 0x80, sizeof(Config)
  AudioConfig config;        // 0x004..0x00a  mask, lengths, counter
  DSUINT8 stateTag;          // 0x00b  0x10 | 0x80
  DSUINT8 stateLength;       // 0x00c  63
  State state;               // 0x00d..0x04b
  PcmPacket hapticsPacket;   // 0x04c..0x08d  0x12, length 64, 6 kHz PCM
  DSUINT8 speakerTagValue;   // 0x08e  0x13 = membrane, 0x16 = jack (+0x80)
  DSUINT8 speakerLength;     // 0x08f  200
  DSUINT8 opus[200];         // 0x090..0x157  Opus 48 kHz stereo CBR 160 kbps
  DSUINT8 padding[50];       // 0x158..0x189
  DSUINT32 crc;              // 0x18a..0x18d  seed 0xA2

public:
#ifdef DS_HAVE_OPUS
  // PCM-facing encoder; libopus types stay contained in this block.
  struct Encoder {
    ::OpusEncoder *encoder;

    constexpr Encoder() : encoder(nullptr) {}

    ~Encoder() { close(); }

    Encoder(const Encoder &)            = delete;
    Encoder &operator=(const Encoder &) = delete;

    bool isValid() const { return encoder != nullptr; }

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
#endif

  // +0x08e: 0x13 for the membrane speaker, 0x16 for the headphone jack.
  enum class SpeakerTag : uint8_t {
    Speaker = static_cast<uint8_t>(SubPacketId::Speaker),
    Jack    = static_cast<uint8_t>(SubPacketId::Jack),
  };

private:
  static constexpr ReportId reportId      = ReportId::Audio;
  static constexpr uint8_t crcSeed        = 0xA2;
  static constexpr uint8_t stateSize      = 63;
  static constexpr uint8_t opusSize       = 200;
  static constexpr uint8_t packetSizedBit = 0x80;
  static constexpr uint8_t packetPid(SubPacketId id) {
    return static_cast<uint8_t>(id) | packetSizedBit;
  }
  constexpr void setSeqTag(uint8_t value) { seqTag = value; }
  constexpr void setCounter(uint8_t value) { config.counter = value; }

  constexpr ConfigMask enableMask() const { return config.mask; }

  constexpr SpeakerTag speakerTag() const {
    return static_cast<SpeakerTag>(static_cast<uint8_t>(speakerTagValue) & 0x7F);
  }

  constexpr bool isValid() const {
    return reportIdValue == reportId && crc == internal::crc_of(*this, crcSeed);
  }

public:
  static constexpr uint8_t hapticSize = 64;
  // The Opus side of the speaker block: 480 samples per frame at 48 kHz (10 ms,
  // CBR 160 kbps = 200 bytes) fed from 512-sample decoder blocks.
  static constexpr uint16_t pcmFrames = 480;

  constexpr OutputBody &stateBody() { return state.body; }
  constexpr const OutputBody &stateBody() const { return state.body; }

  constexpr explicit AudioReport(SpeakerTag tagValue, ConfigMask mask,
                                 OutputReport::Sequence &sequence) {
    reportIdValue = reportId;
    configHeader.set(SubPacketId::Config, sizeof(AudioConfig));
    config.mask = mask;
    for (unsigned i = 0; i < 5; ++i) config.hapticLengths[i] = hapticSize;
    stateTag    = packetPid(SubPacketId::State);
    stateLength = stateSize;
    hapticsPacket.setHeader(SubPacketId::Data, hapticSize);
    speakerTagValue =
        packetPid(tagValue == SpeakerTag::Speaker ? SubPacketId::Speaker : SubPacketId::Jack);
    speakerLength = opusSize;
    state         = State{};
    for (auto &byte : padding) byte = 0;
    for (auto &byte : opus) byte = 0;
    crc            = 0;
    seqTag         = sequence.next();
    config.counter = sequence.nextCounter();
  }

  template <typename HapticFill>
  constexpr void setHaptics(HapticFill fill) {
    hapticsPacket.setHeader(SubPacketId::Data, hapticSize);
    hapticsPacket.fillSamples(fill);
  }

#ifdef DS_HAVE_OPUS
  bool setPcm(const float *pcm, int frames, Encoder &encoder) {
    uint8_t payload[opusSize];
    int n = encoder.encode(pcm, frames, payload, static_cast<int>(sizeof(payload)));
    if (n < 0) n = 0;
    setOpus(payload, static_cast<size_t>(n));
    return n > 0;
  }
#endif

  constexpr void setOpus(const uint8_t *data, size_t len) {
    size_t n = len < opusSize ? len : opusSize;
    for (size_t i = 0; i < n; ++i) opus[i] = data[i];
    for (size_t i = n; i < opusSize; ++i) opus[i] = 0;
  }

  constexpr void sign() { crc = internal::crc_of(*this, crcSeed); }
};
static_assert(sizeof(AudioReport) == 398, "0x36 must stay 398 bytes");

namespace internal {
struct WireProbe {
private:
  static constexpr uint8_t triggerByte(const OutputBody::TriggerEffect &effect, size_t i) {
    return i == 0 ? static_cast<uint8_t>(effect.mode) : effect.parameters[i - 1];
  }

public:
  static constexpr bool all() {
    static_assert(sizeof(AudioReport) == 398, "0x36 must stay 398 bytes");
    static_assert(sizeof(AudioReport::State) == 63, "state must stay 63 bytes");
    static_assert(sizeof(OutputBody) == 47, "output body must stay 47 bytes");

    constexpr uint8_t levels[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    constexpr auto feedback      = OutputBody::TriggerEffect::multiplePositionFeedback(levels);
    static_assert(OutputBody::TriggerEffect::off().isValid());
    static_assert(triggerByte(feedback, 0) == 0x21);
    static_assert(triggerByte(feedback, 1) == 0xFF && triggerByte(feedback, 2) == 0x03);
    static_assert(triggerByte(feedback, 3) == 0 && triggerByte(feedback, 6) == 0);

    constexpr auto weapon = OutputBody::TriggerEffect::weapon(2, 8, 8);
    static_assert(triggerByte(weapon, 0) == 0x25);
    static_assert(triggerByte(weapon, 1) == 0x04 && triggerByte(weapon, 2) == 0x01);
    static_assert(triggerByte(weapon, 3) == 7);

    constexpr uint8_t amplitudes[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    constexpr auto vibration = OutputBody::TriggerEffect::multiplePositionVibration(25, amplitudes);
    static_assert(triggerByte(vibration, 0) == 0x26);
    static_assert(triggerByte(vibration, 1) == 0x01 && triggerByte(vibration, 9) == 25);

    constexpr auto bow = OutputBody::TriggerEffect::bow(2, 8, 6, 4);
    static_assert(triggerByte(bow, 0) == 0x22);
    static_assert(triggerByte(bow, 1) == 0x04 && triggerByte(bow, 2) == 0x01);
    static_assert(triggerByte(bow, 3) == 0x1D);

    constexpr auto galloping = OutputBody::TriggerEffect::galloping(2, 8, 2, 4, 25);
    static_assert(triggerByte(galloping, 0) == 0x23);
    static_assert(triggerByte(galloping, 4) == 25);

    constexpr auto machine = OutputBody::TriggerEffect::machine(2, 8, 6, 4, 25, 10);
    static_assert(triggerByte(machine, 0) == 0x27);
    static_assert(triggerByte(machine, 4) == 25 && triggerByte(machine, 5) == 10);

    constexpr AudioReport report = [] {
      OutputReport::Sequence sequence;
      return AudioReport(AudioReport::SpeakerTag::Jack, AudioReport::ConfigMask::SpeakerOnly,
                         sequence);
    }();
    static_assert(report.reportIdValue == ReportId::Audio);
    static_assert(report.seqTag == 0x00);
    static_assert(report.configHeader.pid() == 0x91);
    static_assert(report.configHeader.length() == sizeof(AudioReport::AudioConfig));
    static_assert(report.enableMask() == AudioReport::ConfigMask::SpeakerOnly);
    static_assert(report.config.hapticLengths[0] == 64 && report.config.hapticLengths[4] == 64);
    static_assert(report.config.counter == 0);
    static_assert(report.stateTag == 0x90);
    static_assert(report.stateLength == 63);
    static_assert(report.stateBody().validFlag0 == 0);
    static_assert(report.stateBody().validFlag1 == 0);
    static_assert(report.speakerTag() == AudioReport::SpeakerTag::Jack);
    static_assert(report.speakerLength == 200);
    static_assert(report.opus[0] == 0);

    return true;
  }
};

// ===========================================================================
// Feature reports — the configuration channel
// ===========================================================================
//
// Feature reports use HID GET/SET reports; Bluetooth signs with seed 0xA3.
// 0x05 calibration, 0x09 pairing, 0x20 firmware, 0x80/0x81 commands. Edge
// factory/profile ranges are outside the modeled subset.

// Calibration math from the Swift schema. The feature report stores raw
// per-axis bias and plus/minus endpoints; converting them to the coefficients
// below is what turns raw IMU counts into usable units.
struct Calibration {
  using ReportId = internal::ReportId;

public:
  struct ImuCalibration {
    struct AxisCalibration {
      int bias;
      int numerator;
      int denominator;

      constexpr AxisCalibration() : bias(0), numerator(0), denominator(1) {}
      constexpr AxisCalibration(int biasValue, int numeratorValue, int denominatorValue)
          : bias(biasValue), numerator(numeratorValue), denominator(denominatorValue) {}

      constexpr int apply(int raw) const {
        if (denominator == 0) return 0;
        return numerator * (raw - bias) / denominator;
      }

      static constexpr AxisCalibration fallback(int range) { return {0, range, 32767}; }
    };

    static constexpr int gyroResolutionPerDps = 1024;
    static constexpr int accelResolutionPerG  = 8192;
    static constexpr int accelRange           = 4 * accelResolutionPerG;
    static constexpr int gyroRange            = 2048 * gyroResolutionPerDps;

    AxisCalibration gyro[3];
    AxisCalibration accel[3];
  };
  static_assert(sizeof(ImuCalibration) == 72, "IMU calibration must stay 72 bytes");

protected:
  ReportId reportIdValue; // 0x00  ReportId::FeatureCalibration
  // 0x01..0x22: seventeen little-endian int16: three gyro biases, then
  // plus/minus at 2x speed for each axis, then gyro speed limits, then accel
  // plus/minus at 1 g per axis.
  DSINT16 gyroPitchBias;
  DSINT16 gyroYawBias;
  DSINT16 gyroRollBias;
  DSINT16 gyroPitchPlus;
  DSINT16 gyroPitchMinus;
  DSINT16 gyroYawPlus;
  DSINT16 gyroYawMinus;
  DSINT16 gyroRollPlus;
  DSINT16 gyroRollMinus;
  DSINT16 gyroSpeedPlus;
  DSINT16 gyroSpeedMinus;
  DSINT16 accXPlus;
  DSINT16 accXMinus;
  DSINT16 accYPlus;
  DSINT16 accYMinus;
  DSINT16 accZPlus;
  DSINT16 accZMinus;
  DSUINT8 unknown35[6]; // 0x23..0x28  not used by the kernel

public:
  static constexpr ReportId reportId = ReportId::FeatureCalibration;

  constexpr ImuCalibration imu() const {
    ImuCalibration result{};
    int speed_2x       = gyroSpeedPlus + gyroSpeedMinus;
    int gyro_numerator = speed_2x * ImuCalibration::gyroResolutionPerDps;

    int pitch_bias = gyroPitchBias;
    int yaw_bias   = gyroYawBias;
    int roll_bias  = gyroRollBias;
    result.gyro[0] = {0, gyro_numerator, gyroPitchPlus - pitch_bias + gyroPitchMinus - pitch_bias};
    result.gyro[1] = {0, gyro_numerator, gyroYawPlus - yaw_bias + gyroYawMinus - yaw_bias};
    result.gyro[2] = {0, gyro_numerator, gyroRollPlus - roll_bias + gyroRollMinus - roll_bias};
    for (auto &axis : result.gyro)
      if (axis.denominator == 0)
        axis = ImuCalibration::AxisCalibration::fallback(ImuCalibration::gyroRange);

    auto accel_axis = [](int plus, int minus) {
      int range_2g = plus - minus;
      if (range_2g == 0)
        return ImuCalibration::AxisCalibration::fallback(ImuCalibration::accelRange);
      return ImuCalibration::AxisCalibration{plus - range_2g / 2,
                                             2 * ImuCalibration::accelResolutionPerG, range_2g};
    };
    result.accel[0] = accel_axis(accXPlus, accXMinus);
    result.accel[1] = accel_axis(accYPlus, accYMinus);
    result.accel[2] = accel_axis(accZPlus, accZMinus);
    return result;
  }
};
static_assert(sizeof(Calibration) == 41, "0x05 must stay 41 bytes");

struct Pairing {
private:
  using DSUINT8  = internal::DSUINT8;
  using DSUINT32 = internal::DSUINT32;
  using ReportId = internal::ReportId;

protected:
  ReportId reportIdValue; // 0x00       ReportId::FeaturePairing
  DSUINT8 mac[6];         // 0x01..0x06 controller address, reversed
  DSUINT8 unknown07[3];   // 0x07..0x09
  DSUINT8 hostMac[6];     // 0x0a..0x0f host address, reversed
  DSUINT32 crc;           // 0x10..0x13 CRC-32 seed 0xA3 on BT, zero on USB

public:
  static constexpr ReportId reportId = ReportId::FeaturePairing;
};
static_assert(sizeof(Pairing) == 20, "0x09 must stay 20 bytes");

} // namespace internal

struct Firmware {
private:
  using DSUINT8  = internal::DSUINT8;
  using DSUINT16 = internal::DSUINT16;
  using DSUINT32 = internal::DSUINT32;
  using ReportId = internal::ReportId;

protected:
  ReportId reportIdValue;   // 0x00  ReportId::FeatureFirmware
  DSUINT8 buildDate[11];    // 0x01..0x0b
  DSUINT8 buildTime[8];     // 0x0c..0x13
  DSUINT16 fwType;          // 0x14..0x15
  DSUINT16 swSeries;        // 0x16..0x17
  DSUINT32 hardwareInfo;    // 0x18..0x1b  variation/generation
  DSUINT32 firmwareVersion; // 0x1c..0x1f
  DSUINT8 deviceInfo[12];   // 0x20..0x2b
  DSUINT16 updateVersion;   // 0x2c..0x2d
  DSUINT8 updateImageInfo;  // 0x2e
  DSUINT8 updateUnk;        // 0x2f
  DSUINT32 fwVersion1;      // 0x30..0x33  SBL
  DSUINT32 fwVersion2;      // 0x34..0x37  Venom
  DSUINT32 fwVersion3;      // 0x38..0x3b  Spider DSP
  DSUINT32 crc;             // 0x3c..0x3f  CRC-32, seed 0xA3

public:
  // Non-owning byte view into one of the fixed wire fields below.
  struct String {
    template <typename Type, size_t n>
    constexpr explicit String(Type (&value_)[n])
        : value(reinterpret_cast<const uint8_t *>(value_)), length(n) {}

    const uint8_t *value;
    size_t length;
  };

  static constexpr ReportId reportId                  = ReportId::FeatureFirmware;
  static constexpr uint8_t crcSeed                    = 0xA3;
  static constexpr uint16_t vibrationV2MinimumVersion = (2 << 8) | 21;

  constexpr Firmware() : reportIdValue(reportId) {}

  constexpr const String buildDateText() const { return String(buildDate); }
  constexpr const String buildTimeText() const { return String(buildTime); }
  constexpr const String deviceInfoText() const { return String(deviceInfo); }
  constexpr uint16_t typeValue() const { return fwType; }
  constexpr uint16_t seriesValue() const { return swSeries; }
  constexpr uint32_t hardwareInfoValue() const { return hardwareInfo; }
  constexpr uint32_t firmwareVersionValue() const { return firmwareVersion; }
  constexpr uint16_t updateVersionValue() const { return updateVersion; }
  constexpr uint8_t updateImageInfoValue() const { return updateImageInfo; }
  constexpr uint8_t updateUnkValue() const { return updateUnk; }
  constexpr uint32_t fwVersion1Value() const { return fwVersion1; }
  constexpr uint32_t fwVersion2Value() const { return fwVersion2; }
  constexpr uint32_t fwVersion3Value() const { return fwVersion3; }
  constexpr uint32_t crcValue() const { return crc; }

  constexpr bool usesVibrationV2() const { return updateVersion >= vibrationV2MinimumVersion; }

  constexpr int buildYear() const {
    auto date = buildDateText();
    if (date.length < 4) return 0;
    int year = 0;
    for (size_t i = date.length - 4; i < date.length; ++i) {
      if (date.value[i] < '0' || date.value[i] > '9') return 0;
      year = year * 10 + (date.value[i] - '0');
    }
    return year;
  }

  constexpr const char *boardModel() const {
    switch ((this->hardwareInfoValue() >> 8) & 0xFF) {
    case 0x03: return "BDM-010";
    case 0x04: return "BDM-020";
    case 0x05: return "BDM-030";
    case 0x06: return "BDM-040";
    case 0x07:
    case 0x08: return "BDM-050";
    case 0x09: return "BDM-060R";
    case 0x11: return "BDM-060M";
    case 0x13: return "BDM-060X";
    default: return "Unknown";
    }
  }

  constexpr const char *deviceType(bool finetuneValid, int currentYear) const {
    if (!finetuneValid) return "clone";
    int year = buildYear();
    if (year == 0 || year > currentYear) return "clone";
    return "original";
  }

  // Bluetooth feature reports carry the CRC; the USB feature transfer does not.
  constexpr bool isValid(bool isUsb) const {
    return reportIdValue == reportId && (isUsb || crc == internal::crc_of(*this, crcSeed));
  }
};
static_assert(sizeof(Firmware) == 64, "0x20 must stay 64 bytes");

// ===========================================================================
// 0x32: Bluetooth haptics container
//
// 64 signed 8-bit samples at 3 kHz stereo. This is the haptics path, not the
// speaker path; 0x36 carries the 6 kHz haptics and Opus speaker streams.
// ===========================================================================
struct HapticsReport {
private:
  using DSUINT8  = internal::DSUINT8;
  using DSINT8   = internal::DSINT8;
  using DSUINT32 = internal::DSUINT32;
  using ReportId = internal::ReportId;

public:
  using PacketHeader = AudioReport::PacketHeader;
  using ConfigMask   = AudioReport::ConfigMask;
  using PcmPacket    = AudioReport::PcmPacket;

  struct HapticsConfigPacket {
  private:
    struct HapticsConfig {
    protected:
      ConfigMask mask;
      DSUINT8 route; // unconfirmed; suspected haptics/speaker selector
      DSUINT8 reserved[3];
      DSUINT8 unknown5; // 0xFF; meaning unconfirmed
      DSUINT8 counter;  // increments once per 10.667 ms frame
    public:
      constexpr explicit HapticsConfig(uint8_t sequence)
          : mask(ConfigMask::SpeakerOnly), route{}, reserved{}, unknown5(0xFF), counter(sequence) {}
      HapticsConfig(const HapticsConfig &)            = delete;
      HapticsConfig &operator=(const HapticsConfig &) = delete;
    };
    static_assert(sizeof(HapticsConfig) == 7, "0x32 haptics config must stay 7 bytes");

  private:
    PacketHeader headerValue;
    HapticsConfig configValue;

  public:
    constexpr explicit HapticsConfigPacket(uint8_t sequence)
        : headerValue(AudioReport::SubPacketId::Config, sizeof(HapticsConfig)),
          configValue(sequence) {}
    HapticsConfigPacket(const HapticsConfigPacket &)            = delete;
    HapticsConfigPacket &operator=(const HapticsConfigPacket &) = delete;
  };
  static_assert(sizeof(HapticsConfigPacket) == 9, "0x32 config packet must stay 9 bytes");

protected:
  ReportId reportId;                // 0x000
  DSUINT8 tagValue;                 // 0x001  low nibble tag, high nibble sequence
  HapticsConfigPacket configPacket; // 0x002..0x00a
  PcmPacket hapticsPacket;          // 0x00b..0x04c
  DSUINT8 padding[61];              // 0x04d..0x089  container padding
  DSUINT32 crc;                     // 0x08a..0x08d  CRC-32, seed 0xA2

public:
  static constexpr uint32_t sampleRate = 3000;
  static constexpr size_t sampleBytes  = AudioReport::hapticSize;

  static_assert(sizeof(PcmPacket) == 66, "0x32 pcm packet must stay 66 bytes");

  // The 0x32 wire format is signed 8-bit PCM.
  constexpr explicit HapticsReport(OutputReport::Sequence &sequence)
      : configPacket(sequence.nextCounter()) {
    reportId = ReportId::Haptics;
    tagValue = 0;
    for (auto &byte : padding) byte = 0;
  }
  HapticsReport(const HapticsReport &)            = delete;
  HapticsReport &operator=(const HapticsReport &) = delete;

  template <typename HapticFill>
  constexpr void setHaptics(HapticFill fill) {
    hapticsPacket.setHeader(AudioReport::SubPacketId::Data, AudioReport::hapticSize);
    hapticsPacket.fillSamples(fill);
  }

  void sign() { crc = internal::crc_of(*this, AudioReport::crcSeed); }
};
static_assert(sizeof(HapticsReport) == 142, "0x32 must stay 142 bytes");

// ===========================================================================
// Microphone frames inside the 0x31 input stream
// ===========================================================================
//
// The controller has no audio class over Bluetooth: the microphone is
// multiplexed into the same HID reports as the gamepad, and its audio is Opus,
// not PCM. Byte 3 is the constant Opus TOC 0xD4; a state report has the
// sequence/tagValue nibbles there instead.
//
//   0x31 seq .. 0xD4 | one Opus frame | CRC-32 (4 bytes, seed 0xA1)
//
// Opus, 48 kHz, mono, 480 samples (10 ms), about 100 packets per second.
//
// Turning capture on takes three writes in the outgoing 0x36:
//   1. powerSaveControl (+0x09) bit 4 = 0 AND validFlag1 bit 1 claims the
//      byte (without the claim the unmute is inert);
//   2. enable mask (0x36 byte 4) bit 0 set: 0xFF instead of 0xFE;
//   3. micVolume (+0x06) = 0x40, claimed by validFlag0 bit 6.
// Step 2 is the tap; step 1 is the one people forget.
//
// It disturbs the gamepad input: these frames carry the same report id as the
// state report, so anything that assumes every 0x31 is 78 bytes reads Opus
// bytes as sticks and buttons. The kernel ignores them, so there is no ALSA
// capture device; decoding them is a host-side job. Duplex roughly doubles the
// link traffic (speaker down and mic up are each ~160 kbps of Opus).

struct MicFrame {
#ifdef DS_HAVE_OPUS
  // PCM-facing decoder for the Opus mic packet.
  struct Decoder {
    ::OpusDecoder *decoder;

    constexpr Decoder() : decoder(nullptr) {}

    ~Decoder() { close(); }

    Decoder(const Decoder &)            = delete;
    Decoder &operator=(const Decoder &) = delete;

    bool isValid() const { return decoder != nullptr; }

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
#endif

private:
  using ReportId                     = internal::ReportId;
  static constexpr ReportId reportId = ReportId::StateBluetooth;
  static constexpr uint8_t toc       = 0xD4;
  static constexpr size_t crcBytes   = 4;

public:
  static constexpr uint32_t sampleRate = 48000;
  static constexpr uint32_t samples    = 480;

#ifdef DS_FUZZER_BUILD
  static void init(uint8_t *buf, size_t len) {
    if (!buf || len < 3 + crcBytes) return;
    memset(buf, 0, len);
    buf[0] = static_cast<uint8_t>(reportId);
    buf[3] = toc;
  }
#endif

  static const MicFrame *asFrame(const uint8_t *buf, size_t len) {
    if (!buf || len <= 3 + crcBytes || buf[0] != static_cast<uint8_t>(reportId) || buf[3] != toc)
      return nullptr;
    return reinterpret_cast<const MicFrame *>(buf);
  }

  // The Opus packet: everything after the TOC byte, minus the trailing CRC.
  static const uint8_t *payload(const uint8_t *buf, size_t len, size_t *out_len) {
    if (!asFrame(buf, len)) return nullptr;
    *out_len = len - 3 - crcBytes;
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

// --- initialized reports, triggers and byte copy ---------------------------

static_assert(internal::WireProbe::all());

} // namespace wire_test
} // namespace internal
} // namespace dualsense
