// padwave_wire.h — the DualSense reports as structs, the way HarfBuzz declares
// its OpenType tables: the struct *is* the on-wire layout, you fill its fields
// (or read them out of it), and bit values are enums rather than shifts at the
// call site.
//
// The host is little-endian and so is the controller, so a packed struct can be
// written and read with memcpy; the static asserts below are the contract.
//
//   0x36 | seq<<4 | 0x91 7 mask 64 64 64 64 64 counter
//        | 0x90 63 <63-byte output state>
//        | 0x92 64 <64 bytes of int8 haptic PCM>
//        | 0x93|0x96 200 <Opus frame>
//        | 50 bytes padding
//        | CRC-32, seed 0xA2, little-endian, over the first 394 bytes
#ifndef PADWAVE_WIRE_H
#define PADWAVE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zlib.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "padwave_wire.h assumes a little-endian host"
#endif

#define PW_0X36_SIZE 398
#define PW_0X36_STATE_SIZE 63
#define PW_0X36_HAPTIC_SIZE 64
#define PW_0X36_OPUS_SIZE 200
#define PW_0X36_CRC_SEED 0xA2
#define PW_0X36_CRC_OFFSET (PW_0X36_SIZE - 4)

// -- flags and small enumerations -------------------------------------------

enum pw_valid_flag0 {
  PW_VALID0_RUMBLE = 0x0001u,
  PW_VALID0_HAPTICS = 0x0002u,
  PW_VALID0_RIGHT_TRIGGER = 0x0004u,
  PW_VALID0_LEFT_TRIGGER = 0x0008u,
  PW_VALID0_HEADPHONE_VOLUME = 0x0010u,
  PW_VALID0_SPEAKER_VOLUME = 0x0020u,
  PW_VALID0_MIC_VOLUME = 0x0040u,
  PW_VALID0_AUDIO_ROUTING = 0x0080u,
};

enum pw_valid_flag1 {
  PW_VALID1_MUTE_LED = 0x0001u,
  PW_VALID1_POWER_SAVE = 0x0002u,
  PW_VALID1_LIGHTBAR = 0x0004u,
  PW_VALID1_RESET_LIGHTS = 0x0008u,
  PW_VALID1_PLAYER_LEDS = 0x0010u,
  PW_VALID1_HAPTIC_LPF = 0x0020u,
  PW_VALID1_MOTOR_POWER_LEVEL = 0x0040u,
  PW_VALID1_AUDIO_CONTROL2 = 0x0080u,
};

enum pw_valid_flag2 {
  PW_VALID2_BRIGHTNESS = 0x0001u,
  PW_VALID2_COLOR_FADE = 0x0002u,
  PW_VALID2_IMPROVED_RUMBLE = 0x0004u,
};

enum pw_power_save {
  PW_POWER_SAVE_MIC_MUTE = 0x0010u,
  PW_POWER_SAVE_AUDIO_MUTE = 0x0040u,
};

enum pw_audio_control {
  PW_AUDIO_CONTROL_HEADPHONES = 0x00u,
  PW_AUDIO_CONTROL_SPEAKER = 0x09u,
};

enum pw_enable_mask {
  PW_ENABLE_MICROPHONE = 0x01u, /* clear in the speaker-only mask */
  PW_ENABLE_SPEAKER_ONLY = 0xFEu,
  PW_ENABLE_SPEAKER_AND_MIC = 0xFFu,
};

enum pw_speaker_tag {
  PW_SPEAKER_TAG = 0x13u, /* membrane */
  PW_JACK_TAG = 0x16u,    /* headphone jack */
};

// -- the output state -------------------------------------------------------

// The 63-byte state snapshot. Comments give the payload offset (report + 0x0D).
struct pw_output_state {
  uint8_t valid_flag0;              /* +0x00  enum pw_valid_flag0 */
  uint8_t valid_flag1;              /* +0x01  enum pw_valid_flag1 */
  uint8_t motor_right;              /* +0x02 */
  uint8_t motor_left;               /* +0x03 */
  uint8_t headphone_volume;         /* +0x04  0x00..0x7f */
  uint8_t speaker_volume;           /* +0x05  0x00..0xff */
  uint8_t mic_volume;               /* +0x06  0x00..0x40 */
  uint8_t audio_control;            /* +0x07  enum pw_audio_control */
  uint8_t mute_button_led;          /* +0x08 */
  uint8_t power_save_control;       /* +0x09  enum pw_power_save */
  uint8_t right_trigger_mode;       /* +0x0a */
  uint8_t right_trigger_params[10]; /* +0x0b */
  uint8_t left_trigger_mode;        /* +0x15 */
  uint8_t left_trigger_params[10];  /* +0x16 */
  uint8_t reserved_32[4];           /* +0x20 */
  uint8_t haptic_volume;            /* +0x24 */
  uint8_t audio_control2;           /* +0x25 */
  uint8_t valid_flag2;              /* +0x26  enum pw_valid_flag2 */
  uint8_t reserved_39[2];           /* +0x27 */
  uint8_t lightbar_setup;           /* +0x29 */
  uint8_t led_brightness;           /* +0x2a */
  uint8_t player_leds;              /* +0x2b */
  uint8_t lightbar_red;             /* +0x2c */
  uint8_t lightbar_green;           /* +0x2d */
  uint8_t lightbar_blue;            /* +0x2e */
  uint8_t reserved4[16];            /* +0x2f */
} __attribute__((packed));

// -- the audio report -------------------------------------------------------

// Init it, fill what you own, sign it, write it.
struct pw_0x36 {
  uint8_t report_id;          /* 0x36 */
  uint8_t seq;                /* high nibble, low nibble zero */
  uint8_t config_tag;         /* 0x91 */
  uint8_t config_length;      /* 7 */
  uint8_t enable_mask;        /* enum pw_enable_mask */
  uint8_t haptic_lengths[5];  /* 64 each */
  uint8_t counter;            /* free-running 8-bit counter */
  uint8_t state_tag;          /* 0x90 */
  uint8_t state_length;       /* 63 */
  struct pw_output_state state;
  uint8_t haptics_tag;        /* 0x92 */
  uint8_t haptics_length;     /* 64 */
  int8_t haptics_pcm[PW_0X36_HAPTIC_SIZE];
  uint8_t speaker_tag;        /* 0x93 | 0x80 or 0x96 | 0x80 */
  uint8_t speaker_length;     /* 200 */
  uint8_t opus[PW_0X36_OPUS_SIZE];
  uint8_t padding[50];
  uint32_t crc;               /* seed 0xA2 over the first 394 bytes */
} __attribute__((packed));

_Static_assert(sizeof(struct pw_output_state) == PW_0X36_STATE_SIZE,
               "the state snapshot must stay 63 bytes");
_Static_assert(sizeof(struct pw_0x36) == PW_0X36_SIZE, "the report must stay 398 bytes");

// Fills the constant parts. The caller then sets seq, counter and whatever
// state fields it is allowed to own — that is what the valid flags are for.
static inline void pw_0x36_init(struct pw_0x36 *r, bool speaker) {
  memset(r, 0, sizeof(*r));
  r->report_id = 0x36;
  r->config_tag = 0x11 | 0x80;
  r->config_length = 7;
  r->enable_mask = PW_ENABLE_SPEAKER_ONLY;
  for (int i = 0; i < 5; i++) r->haptic_lengths[i] = PW_0X36_HAPTIC_SIZE;
  r->state_tag = 0x10 | 0x80;
  r->state_length = PW_0X36_STATE_SIZE;
  r->haptics_tag = 0x12 | 0x80;
  r->haptics_length = PW_0X36_HAPTIC_SIZE;
  r->speaker_tag = (uint8_t)((speaker ? PW_SPEAKER_TAG : PW_JACK_TAG) | 0x80);
  r->speaker_length = PW_0X36_OPUS_SIZE;
}

// A snapshot that claims only what the audio path writes. Claiming more here is
// how one writer silently owns a feature another writer is using.
static inline void pw_output_state_init(struct pw_output_state *s, enum pw_audio_control routing) {
  memset(s, 0, sizeof(*s));
  s->valid_flag0 = PW_VALID0_HEADPHONE_VOLUME | PW_VALID0_SPEAKER_VOLUME | PW_VALID0_AUDIO_ROUTING;
  s->valid_flag1 = PW_VALID1_AUDIO_CONTROL2;
  s->headphone_volume = 0x7F;
  s->speaker_volume = 0x64;
  s->audio_control = (uint8_t)routing;
  s->audio_control2 = 0x0A;
}

// The CRC is the last thing the controller checks, so it is signed here rather
// than by whoever filled the fields.
static inline void pw_0x36_sign(struct pw_0x36 *r) {
  uint8_t seed = PW_0X36_CRC_SEED;
  uLong crc = crc32(0L, &seed, 1);
  crc = crc32(crc, (const Bytef *)r, PW_0X36_CRC_OFFSET);
  r->crc = (uint32_t)crc;
}

static inline bool pw_0x36_valid(const struct pw_0x36 *r) {
  uint8_t seed = PW_0X36_CRC_SEED;
  uLong crc = crc32(0L, &seed, 1);
  crc = crc32(crc, (const Bytef *)r, PW_0X36_CRC_OFFSET);
  return r->report_id == 0x36 && r->crc == (uint32_t)crc;
}

// Reads a report back out of bytes — useful for tests and for inspecting what
// was sent.
static inline bool pw_0x36_parse(const uint8_t *buf, size_t len, struct pw_0x36 *out) {
  if (len != PW_0X36_SIZE) return false;
  memcpy(out, buf, PW_0X36_SIZE);
  return pw_0x36_valid(out);
}

static inline size_t pw_0x36_serialize(uint8_t *out, size_t cap, const struct pw_0x36 *r) {
  if (cap < PW_0X36_SIZE || !pw_0x36_valid(r)) return 0;
  memcpy(out, r, PW_0X36_SIZE);
  return PW_0X36_SIZE;
}

#endif /* PADWAVE_WIRE_H */
