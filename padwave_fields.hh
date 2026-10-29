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

using Field = padwave::OutputBody::Field;

struct u8_field {
  const char *name;
  Field field;
};

struct array_field {
  const char *name;
  Field field;
  size_t length;
};

struct flag {
  const char *name;
  uint8_t bit;
};

inline const u8_field kU8Fields[] = {
    {"valid_flag0", Field::ValidFlag0},
    {"valid_flag1", Field::ValidFlag1},
    {"motor_right", Field::MotorRight},
    {"motor_left", Field::MotorLeft},
    {"headphone_volume", Field::HeadphoneVolume},
    {"speaker_volume", Field::SpeakerVolume},
    {"mic_volume", Field::MicVolume},
    {"audio_control", Field::AudioRouting},
    {"mute_button_led", Field::MuteButtonLed},
    {"power_save_control", Field::PowerSaveControl},
    {"right_trigger_mode", Field::RightTriggerMode},
    {"left_trigger_mode", Field::LeftTriggerMode},
    {"haptic_volume", Field::HapticVolume},
    {"audio_control2", Field::AudioControl2},
    {"valid_flag2", Field::ValidFlag2},
    {"lightbar_setup", Field::LightbarSetup},
    {"led_brightness", Field::LedBrightness},
    {"player_leds", Field::PlayerLedsMask},
    {"lightbar_red", Field::LightbarRed},
    {"lightbar_green", Field::LightbarGreen},
    {"lightbar_blue", Field::LightbarBlue},
};

inline const array_field kArrayFields[] = {
    {"right_trigger_params", Field::RightTriggerParams, 10},
    {"left_trigger_params", Field::LeftTriggerParams, 10},
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
    {"speaker_volume",
     padwave::OutputBody::bit(padwave::OutputBody::ValidFlag0::SpeakerVolume)},
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

inline bool is_flag_field(Field field) {
  return field == Field::ValidFlag0 || field == Field::ValidFlag1 || field == Field::ValidFlag2;
}

// Parse either a raw byte or a comma-separated list of flag names. This is the
// only place that knows the flag spellings; OutputBody only receives the mask.
inline uint8_t parse_flag_mask(Field field, const char *value) {
  const flag *table = nullptr;
  size_t count = 0;
  if (field == Field::ValidFlag0) {
    table = kFlag0;
    count = sizeof(kFlag0) / sizeof(kFlag0[0]);
  } else if (field == Field::ValidFlag1) {
    table = kFlag1;
    count = sizeof(kFlag1) / sizeof(kFlag1[0]);
  } else if (field == Field::ValidFlag2) {
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
      if (is_flag_field(kU8Fields[i].field)) {
        uint8_t bits = parse_flag_mask(kU8Fields[i].field, value);
        if (!body->setFlagWord(kU8Fields[i].field, bits, merge_flags)) return false;
      } else if (!body->setField(kU8Fields[i].field,
                                 static_cast<uint8_t>(strtoul(value, nullptr, 0)))) {
        return false;
      }
      found = true;
      break;
    }
    if (found) continue;

    for (size_t i = 0; i < sizeof(kArrayFields) / sizeof(kArrayFields[0]); i++) {
      if (strcmp(kArrayFields[i].name, name) != 0) continue;
      uint8_t bytes[10] = {};
      if (!hex_to_bytes(value, bytes, kArrayFields[i].length)) {
        size_t index = 0;
        char vcopy[256];
        snprintf(vcopy, sizeof(vcopy), "%s", value);
        char *save_bytes = nullptr;
        for (char *b = strtok_r(vcopy, ",", &save_bytes);
             b && index < kArrayFields[i].length; b = strtok_r(nullptr, ",", &save_bytes))
          bytes[index++] = static_cast<uint8_t>(strtoul(b, nullptr, 0));
      }
      if (!body->setArray(kArrayFields[i].field, bytes, kArrayFields[i].length)) return false;
      found = true;
      break;
    }
    if (!found) return false;
  }
  return true;
}

} // namespace padwave_fields
