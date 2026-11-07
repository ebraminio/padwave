// padwave_fields.hh — the 0x31 field-map parser.
//
// Names are the OutputBody fields; each setter claims its own validity bits.
// The two trigger parameter blocks are hex. Used by the player's 0x31 writer
// and by the field fuzzer.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dualsense.hh"

// Output-body field application, shared by the player's 0x31 writer.
//
// Only the string names and value parsing live here. OutputBody owns the
// storage, setters and validity claims; no offsets escape it.

namespace padwave_fields {

using Body = dualsense::OutputBody;
using Setter = void (Body::*)(uint8_t);
using ArraySetter = void (Body::*)(const uint8_t *);

struct u8_field {
  const char *name;
  Setter set;
};

struct array_field {
  const char *name;
  ArraySetter set;
};

inline const u8_field kU8Fields[] = {
    {"motor_right", &Body::setMotorRight},
    {"motor_left", &Body::setMotorLeft},
    {"headphone_volume", &Body::setHeadphoneVolume},
    {"speaker_volume", &Body::setSpeakerVolume},
    {"mic_volume", &Body::setMicVolume},
    {"audio_control", &Body::setAudioRouting},
    {"mute_button_led", &Body::setMuteButtonLed},
    {"power_save_control", &Body::setPowerSaveControl},
    {"right_trigger_mode", &Body::setRightTriggerMode},
    {"left_trigger_mode", &Body::setLeftTriggerMode},
    {"motor_power_level", &Body::setMotorPowerLevel},
    {"haptic_low_pass_filter", &Body::setHapticLowPassFilter},
    {"speaker_preamp_gain", &Body::setSpeakerPreampGain},
    {"lightbar_setup", nullptr},
};

inline const array_field kArrayFields[] = {
    {"right_trigger_params", &Body::setRightTriggerParams},
    {"left_trigger_params", &Body::setLeftTriggerParams},
};

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
inline bool apply_fields(dualsense::OutputBody *body, const char *text) {
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

    if (strcmp(name, "player_led_brightness") == 0) {
      unsigned level = static_cast<unsigned>(strtoul(value, nullptr, 0));
      if (level > 2) return false;
      body->setPlayerLedBrightness(static_cast<Body::PlayerLedBrightness>(level));
      continue;
    }

    if (strcmp(name, "player_leds") == 0) {
      uint8_t mask = static_cast<uint8_t>(strtoul(value, nullptr, 0));
      body->setPlayerLeds((mask & 0x01) != 0, (mask & 0x02) != 0, (mask & 0x04) != 0,
                          (mask & 0x08) != 0, (mask & 0x10) != 0, (mask & 0x20) != 0);
      continue;
    }

    if (strcmp(name, "lightbar_rgb") == 0) {
      unsigned red = 0, green = 0, blue = 0;
      if (sscanf(value, "%u,%u,%u", &red, &green, &blue) != 3) return false;
      body->setLightbar(static_cast<uint8_t>(red), static_cast<uint8_t>(green),
                        static_cast<uint8_t>(blue));
      continue;
    }

    bool found = false;
    for (size_t i = 0; i < sizeof(kU8Fields) / sizeof(kU8Fields[0]); i++) {
      if (strcmp(kU8Fields[i].name, name) != 0) continue;
      uint8_t byte = static_cast<uint8_t>(strtoul(value, nullptr, 0));
      if (kU8Fields[i].set)
        (body->*(kU8Fields[i].set))(byte);
      else
        body->setLightbarSetup(static_cast<Body::LightbarSetup>(byte));
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
