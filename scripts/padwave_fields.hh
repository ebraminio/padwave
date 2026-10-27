#pragma once
// Output-body field application, shared by the player's 0x31 writer.
//
// Names are the OutputBody members; a flag word's value is a comma list of
// ValidFlag0/1/2 enumerator names; the two trigger parameter blocks are hex.
// Kept out of padwave_output.cc so the single player process can build both
// the 0x36 audio reports and these 0x31 control reports from the same schema.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "padwave_schemas.hh"

namespace padwave_fields {


struct u8_field {
  const char *name;
  size_t offset;
};

struct array_field {
  const char *name;
  size_t offset;
  size_t length;
};

struct flag {
  const char *name;
  uint8_t bit;
};

inline const u8_field kU8Fields[] = {
    {"valid_flag0", offsetof(padwave::OutputBody, valid_flag0)},
    {"valid_flag1", offsetof(padwave::OutputBody, valid_flag1)},
    {"motor_right", offsetof(padwave::OutputBody, motor_right)},
    {"motor_left", offsetof(padwave::OutputBody, motor_left)},
    {"headphone_volume", offsetof(padwave::OutputBody, headphone_volume)},
    {"speaker_volume", offsetof(padwave::OutputBody, speaker_volume)},
    {"mic_volume", offsetof(padwave::OutputBody, mic_volume)},
    {"audio_control", offsetof(padwave::OutputBody, audio_control)},
    {"mute_button_led", offsetof(padwave::OutputBody, mute_button_led)},
    {"power_save_control", offsetof(padwave::OutputBody, power_save_control)},
    {"right_trigger_mode", offsetof(padwave::OutputBody, right_trigger_mode)},
    {"left_trigger_mode", offsetof(padwave::OutputBody, left_trigger_mode)},
    {"haptic_volume", offsetof(padwave::OutputBody, haptic_volume)},
    {"audio_control2", offsetof(padwave::OutputBody, audio_control2)},
    {"valid_flag2", offsetof(padwave::OutputBody, valid_flag2)},
    {"lightbar_setup", offsetof(padwave::OutputBody, lightbar_setup)},
    {"led_brightness", offsetof(padwave::OutputBody, led_brightness)},
    {"player_leds", offsetof(padwave::OutputBody, player_leds)},
    {"lightbar_red", offsetof(padwave::OutputBody, lightbar_red)},
    {"lightbar_green", offsetof(padwave::OutputBody, lightbar_green)},
    {"lightbar_blue", offsetof(padwave::OutputBody, lightbar_blue)},
};

inline const array_field kArrayFields[] = {
    {"right_trigger_params", offsetof(padwave::OutputBody, right_trigger_params), 10},
    {"left_trigger_params", offsetof(padwave::OutputBody, left_trigger_params), 10},
};

// The flag names are the ValidFlag0/1/2 enumerators, lowercased, as the Elixir
// field maps and the UI spell them.
inline const flag kFlag0[] = {
    {"rumble", padwave::OutputBody::Rumble},
    {"haptics", padwave::OutputBody::Haptics},
    {"right_trigger", padwave::OutputBody::RightTrigger},
    {"left_trigger", padwave::OutputBody::LeftTrigger},
    {"headphone_volume", padwave::OutputBody::HeadphoneVolume},
    {"speaker_volume", padwave::OutputBody::SpeakerVolume},
    {"mic_volume", padwave::OutputBody::MicVolume},
    {"audio_routing", padwave::OutputBody::AudioRouting},
};
inline const flag kFlag1[] = {
    {"mute_led", padwave::OutputBody::MuteLed},
    {"power_save", padwave::OutputBody::PowerSave},
    {"lightbar", padwave::OutputBody::Lightbar},
    {"reset_lights", padwave::OutputBody::ResetLights},
    {"player_leds", padwave::OutputBody::PlayerLeds},
    {"haptic_lpf", padwave::OutputBody::HapticLpf},
    {"motor_power_level", padwave::OutputBody::MotorPowerLevel},
    {"audio_control2", padwave::OutputBody::AudioControl2},
};
inline const flag kFlag2[] = {
    {"brightness", padwave::OutputBody::Brightness},
    {"color_fade", padwave::OutputBody::ColorFade},
    {"improved_rumble", padwave::OutputBody::ImprovedRumble},
};

int find_dualsense() {
  for (int i = 0; i < 8; i++) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/hidraw/hidraw%d/device/uevent", i);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    char line[256];
    bool match = false;
    while (fgets(line, sizeof(line), f))
      if (strstr(line, "054C:00000CE6") || strstr(line, "054C:00000DF2")) match = true;
    fclose(f);
    if (!match) continue;
    snprintf(path, sizeof(path), "/dev/hidraw%d", i);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) return fd;
  }
  return -1;
}

bool write_all(int fd, const uint8_t *data, size_t len) {
  size_t put = 0;
  while (put < len) {
    ssize_t n = write(fd, data + put, len - put);
    if (n <= 0) return false;
    put += (size_t)n;
  }
  return true;
}

inline const flag *find_flag(const flag *table, size_t count, const char *name) {
  for (size_t i = 0; i < count; i++)
    if (strcmp(table[i].name, name) == 0) return &table[i];
  return nullptr;
}

inline void set_flag_word(padwave::OutputBody *body, const u8_field *field, const char *value,
                          bool merge) {
  const flag *table = nullptr;
  size_t count = 0;
  if (field->offset == offsetof(padwave::OutputBody, valid_flag0)) {
    table = kFlag0;
    count = sizeof(kFlag0) / sizeof(kFlag0[0]);
  } else if (field->offset == offsetof(padwave::OutputBody, valid_flag1)) {
    table = kFlag1;
    count = sizeof(kFlag1) / sizeof(kFlag1[0]);
  } else {
    table = kFlag2;
    count = sizeof(kFlag2) / sizeof(kFlag2[0]);
  }

  uint8_t bits = 0;
  char copy[512];
  snprintf(copy, sizeof(copy), "%s", value);
  char *save = nullptr;
  for (char *name = strtok_r(copy, ",", &save); name; name = strtok_r(nullptr, ",", &save)) {
    const flag *f = find_flag(table, count, name);
    if (f) bits = (uint8_t)(bits | f->bit);
  }
  uint8_t *word = (uint8_t *)((char *)body + field->offset);
  *word = merge ? (uint8_t)(*word | bits) : bits;
}

inline bool hex_to_bytes(const char *hex, uint8_t *out, size_t len) {
  if (strlen(hex) != len * 2) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned value = 0;
    if (sscanf(hex + i * 2, "%2x", &value) != 1) return false;
    out[i] = (uint8_t)value;
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
      if (strcmp(name, "valid_flag0") == 0 || strcmp(name, "valid_flag1") == 0 ||
          strcmp(name, "valid_flag2") == 0)
        set_flag_word(body, &kU8Fields[i], value, merge_flags);
      else
        *(uint8_t *)((char *)body + kU8Fields[i].offset) = (uint8_t)strtoul(value, nullptr, 0);
      found = true;
      break;
    }
    if (found) continue;

    for (size_t i = 0; i < sizeof(kArrayFields) / sizeof(kArrayFields[0]); i++) {
      if (strcmp(kArrayFields[i].name, name) != 0) continue;
      uint8_t *dst = (uint8_t *)((char *)body + kArrayFields[i].offset);
      if (!hex_to_bytes(value, dst, kArrayFields[i].length)) {
        // comma-separated bytes are also accepted
        size_t index = 0;
        char vcopy[256];
        snprintf(vcopy, sizeof(vcopy), "%s", value);
        char *save_bytes = nullptr;
        for (char *b = strtok_r(vcopy, ",", &save_bytes); b && index < kArrayFields[i].length;
             b = strtok_r(nullptr, ",", &save_bytes))
          dst[index++] = (uint8_t)strtoul(b, nullptr, 0);
      }
      found = true;
      break;
    }
    if (!found) return false;
  }
  return true;
}

} // namespace padwave_fields
