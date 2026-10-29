// padwave_fields.hh — the 0x31 field-map parser.
//
// Names are the OutputBody members; a flag word's value is a comma list of
// ValidFlag0/1/2 enumerator names; the two trigger parameter blocks are hex.
// Used by the player's 0x31 writer and by the field fuzzer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "padwave_wire.hh"

// Output-body field application, shared by the player's 0x31 writer.
//
// Only the string names and flag spellings live here. OutputBody owns the
// storage, field switch and flag-word merge; no offsets escape it.

namespace padwave_fields {

using Body = padwave::OutputBody;
using Setter = void (Body::*)(uint8_t);
using ArraySetter = void (Body::*)(const uint8_t *);

enum class FlagWord : uint8_t { None, Zero, One, Two };

struct u8_field {
  const char *name;
  Setter set;
  Setter merge;
  FlagWord flags;
};

struct array_field {
  const char *name;
  ArraySetter set;
};

struct flag {
  const char *name;
  uint8_t bit;
};

inline const u8_field kU8Fields[] = {
    {"valid_flag0", &Body::setValidFlag0, &Body::mergeValidFlag0, FlagWord::Zero},
    {"valid_flag1", &Body::setValidFlag1, &Body::mergeValidFlag1, FlagWord::One},
    {"motor_right", &Body::setMotorRight, nullptr, FlagWord::None},
    {"motor_left", &Body::setMotorLeft, nullptr, FlagWord::None},
    {"headphone_volume", &Body::setHeadphoneVolume, nullptr, FlagWord::None},
    {"speaker_volume", &Body::setSpeakerVolume, nullptr, FlagWord::None},
    {"mic_volume", &Body::setMicVolume, nullptr, FlagWord::None},
    {"audio_control", &Body::setAudioRouting, nullptr, FlagWord::None},
    {"mute_button_led", &Body::setMuteButtonLed, nullptr, FlagWord::None},
    {"power_save_control", &Body::setPowerSaveControl, nullptr, FlagWord::None},
    {"right_trigger_mode", &Body::setRightTriggerMode, nullptr, FlagWord::None},
    {"left_trigger_mode", &Body::setLeftTriggerMode, nullptr, FlagWord::None},
    {"haptic_volume", &Body::setHapticVolume, nullptr, FlagWord::None},
    {"audio_control2", &Body::setAudioControl2, nullptr, FlagWord::None},
    {"valid_flag2", &Body::setValidFlag2, &Body::mergeValidFlag2, FlagWord::Two},
    {"lightbar_setup", &Body::setLightbarSetup, nullptr, FlagWord::None},
    {"led_brightness", &Body::setLedBrightness, nullptr, FlagWord::None},
    {"player_leds", &Body::setPlayerLedsMask, nullptr, FlagWord::None},
    {"lightbar_red", &Body::setLightbarRed, nullptr, FlagWord::None},
    {"lightbar_green", &Body::setLightbarGreen, nullptr, FlagWord::None},
    {"lightbar_blue", &Body::setLightbarBlue, nullptr, FlagWord::None},
};

inline const array_field kArrayFields[] = {
    {"right_trigger_params", &Body::setRightTriggerParams},
    {"left_trigger_params", &Body::setLeftTriggerParams},
};

// The flag names are the ValidFlag0/1/2 enumerators, lowercased, as the Elixir
// field maps and the UI spell them.
inline const flag kFlag0[] = {
    {"rumble", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag0::Rumble)},
    {"haptics", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag0::Haptics)},
    {"right_trigger", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag0::RightTrigger)},
    {"left_trigger", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag0::LeftTrigger)},
    {"headphone_volume",
     padwave::OutputBody::bit(padwave::OutputBody::ValidFlag0::HeadphoneVolume)},
    {"speaker_volume", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag0::SpeakerVolume)},
    {"mic_volume", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag0::MicVolume)},
    {"audio_routing", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag0::AudioRouting)},
};
inline const flag kFlag1[] = {
    {"mute_led", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag1::MuteLed)},
    {"power_save", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag1::PowerSave)},
    {"lightbar", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag1::Lightbar)},
    {"reset_lights", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag1::ResetLights)},
    {"player_leds", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag1::PlayerLeds)},
    {"haptic_lpf", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag1::HapticLpf)},
    {"motor_power_level",
     padwave::OutputBody::bit(padwave::OutputBody::ValidFlag1::MotorPowerLevel)},
    {"audio_control2", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag1::AudioControl2)},
};
inline const flag kFlag2[] = {
    {"brightness", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag2::Brightness)},
    {"color_fade", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag2::ColorFade)},
    {"improved_rumble", padwave::OutputBody::bit(padwave::OutputBody::ValidFlag2::ImprovedRumble)},
};

inline bool write_all(int fd, const uint8_t *data, size_t len) {
  size_t put = 0;
  while (put < len) {
    ssize_t n = write(fd, data + put, len - put);
    if (n <= 0) return false;
    put += static_cast<size_t>(n);
  }
  return true;
}

inline const flag *find_flag(const flag *table, size_t count, const char *name) {
  for (size_t i = 0; i < count; i++)
    if (strcmp(table[i].name, name) == 0) return &table[i];
  return nullptr;
}

// Parse either a raw byte or a comma-separated list of flag names. This is the
// only place that knows the flag spellings; OutputBody only receives the mask.
inline uint8_t parse_flag_mask(FlagWord kind, const char *value) {
  const flag *table = nullptr;
  size_t count = 0;
  if (kind == FlagWord::Zero) {
    table = kFlag0;
    count = sizeof(kFlag0) / sizeof(kFlag0[0]);
  } else if (kind == FlagWord::One) {
    table = kFlag1;
    count = sizeof(kFlag1) / sizeof(kFlag1[0]);
  } else if (kind == FlagWord::Two) {
    table = kFlag2;
    count = sizeof(kFlag2) / sizeof(kFlag2[0]);
  }

  char *end = nullptr;
  unsigned long raw = strtoul(value, &end, 0);
  if (end && end != value && *end == 0) return static_cast<uint8_t>(raw);

  uint8_t bits = 0;
  char copy[512];
  snprintf(copy, sizeof(copy), "%s", value);
  char *save = nullptr;
  for (char *name = strtok_r(copy, ",", &save); name; name = strtok_r(nullptr, ",", &save)) {
    const flag *f = find_flag(table, count, name);
    if (f) bits = static_cast<uint8_t>(bits | f->bit);
  }
  return bits;
}

inline bool hex_to_bytes(const char *hex, uint8_t *out, size_t len) {
  if (strlen(hex) != len * 2) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned value = 0;
    if (sscanf(hex + i * 2, "%2x", &value) != 1) return false;
    out[i] = static_cast<uint8_t>(value);
  }
  return true;
}

// "fields a=1;b=2,3" -> fills the body, returns false on an unknown field.
inline bool apply_fields(padwave::OutputBody *body, const char *text, bool merge_flags = false) {
  char copy[4096];
  snprintf(copy, sizeof(copy), "%s", text);
  char *save_pairs = nullptr;
  for (char *pair = strtok_r(copy, ";", &save_pairs); pair;
       pair = strtok_r(nullptr, ";", &save_pairs)) {
    char *eq = strchr(pair, '=');
    if (!eq) return false;
    *eq = 0;
    const char *name = pair;
    const char *value = eq + 1;

    bool found = false;
    for (size_t i = 0; i < sizeof(kU8Fields) / sizeof(kU8Fields[0]); i++) {
      if (strcmp(kU8Fields[i].name, name) != 0) continue;
      if (kU8Fields[i].flags != FlagWord::None) {
        uint8_t bits = parse_flag_mask(kU8Fields[i].flags, value);
        if (merge_flags && kU8Fields[i].merge)
          (body->*(kU8Fields[i].merge))(bits);
        else
          (body->*(kU8Fields[i].set))(bits);
      } else {
        (body->*(kU8Fields[i].set))(static_cast<uint8_t>(strtoul(value, nullptr, 0)));
      }
      found = true;
      break;
    }
    if (found) continue;

    for (size_t i = 0; i < sizeof(kArrayFields) / sizeof(kArrayFields[0]); i++) {
      if (strcmp(kArrayFields[i].name, name) != 0) continue;
      uint8_t bytes[10] = {};
      if (!hex_to_bytes(value, bytes, sizeof(bytes))) {
        size_t index = 0;
        char vcopy[256];
        snprintf(vcopy, sizeof(vcopy), "%s", value);
        char *save_bytes = nullptr;
        for (char *b = strtok_r(vcopy, ",", &save_bytes); b && index < sizeof(bytes);
             b = strtok_r(nullptr, ",", &save_bytes))
          bytes[index++] = static_cast<uint8_t>(strtoul(b, nullptr, 0));
      }
      (body->*(kArrayFields[i].set))(bytes);
      found = true;
      break;
    }
    if (!found) return false;
  }
  return true;
}

} // namespace padwave_fields
