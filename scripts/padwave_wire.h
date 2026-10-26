// padwave_wire.h — HarfBuzz-flavoured serialization for the DualSense reports.
//
// The shape is deliberately close to HarfBuzz's `hb_serialize_context_t`: a
// writer that can *fail* instead of writing out of bounds, sub-structures with
// their own length tags, and one entry point per report that lays the fields
// down in the order the controller expects. Nothing here knows about audio or
// files; it only turns a struct of parts into the bytes on the wire.
//
// The 0x36 report is four tagged sub-packets followed by padding and a CRC:
//
//   0x36 | seq<<4 | 0x91 len(7) mask 64 64 64 64 64 counter
//        | 0x90 len(63) <state snapshot>
//        | 0x92 len(64) <haptic PCM>
//        | 0x93|0x96 len(200) <Opus frame>
//        | 50 bytes of padding
//        | CRC-32 (seed 0xA2, little-endian, over the first 394 bytes)
//
// Everything is header-only and C99; a C++ translation unit can include it
// unchanged.
#ifndef PADWAVE_WIRE_H
#define PADWAVE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zlib.h>

#define PW_WIRE_ID 0x36
#define PW_WIRE_SIZE 398
#define PW_WIRE_STATE 63
#define PW_WIRE_HAPTIC 64
#define PW_WIRE_OPUS 200
#define PW_WIRE_CRC_SEED 0xA2
#define PW_WIRE_CRC_BYTES 4

// A bounded writer. Once `failed` is set it stays set and every further write
// is a no-op: callers write the happy path and check once at the end, the same
// contract as hb_serialize_context_t's successful().
typedef struct {
  uint8_t *buf;
  size_t cap;
  size_t len;
  bool failed;
} pw_writer;

static inline pw_writer pw_writer_new(uint8_t *buf, size_t cap) {
  pw_writer w = {.buf = buf, .cap = cap, .len = 0, .failed = false};
  return w;
}

static inline bool pw_reserve(pw_writer *w, size_t n) {
  if (w->failed) return false;
  if (w->len + n > w->cap) {
    w->failed = true;
    return false;
  }
  return true;
}

static inline void pw_u8(pw_writer *w, uint8_t value) {
  if (!pw_reserve(w, 1)) return;
  w->buf[w->len++] = value;
}

static inline void pw_u16le(pw_writer *w, uint16_t value) {
  if (!pw_reserve(w, 2)) return;
  w->buf[w->len++] = (uint8_t)(value & 0xFF);
  w->buf[w->len++] = (uint8_t)(value >> 8);
}

static inline void pw_u32le(pw_writer *w, uint32_t value) {
  if (!pw_reserve(w, 4)) return;
  for (int i = 0; i < 4; i++) w->buf[w->len++] = (uint8_t)(value >> (8 * i));
}

static inline void pw_bytes(pw_writer *w, const void *data, size_t n) {
  if (!pw_reserve(w, n)) return;
  memcpy(w->buf + w->len, data, n);
  w->len += n;
}

static inline void pw_zeros(pw_writer *w, size_t n) {
  if (!pw_reserve(w, n)) return;
  memset(w->buf + w->len, 0, n);
  w->len += n;
}

// A length-tagged sub-packet: the tag keeps its 0x80 "another follows" bit and
// the length is one byte, which is all four sub-packets of 0x36 need.
static inline void pw_subpacket(pw_writer *w, uint8_t tag, const void *payload, uint8_t len) {
  pw_u8(w, (uint8_t)(tag | 0x80));
  pw_u8(w, len);
  if (payload) pw_bytes(w, payload, len);
  else pw_zeros(w, len);
}

static inline bool pw_ok(const pw_writer *w) { return !w->failed && w->len <= w->cap; }

// The CRC is what the controller checks last, so it is appended by the report
// serializer rather than by whoever filled the block.
static inline void pw_crc32(pw_writer *w) {
  uint8_t seed = PW_WIRE_CRC_SEED;
  uLong crc = crc32(0L, &seed, 1);
  crc = crc32(crc, w->buf, w->len);
  pw_u32le(w, (uint32_t)crc);
}

// -- 0x36: the audio report ------------------------------------------------

// The parts of the report, named the way the Elixir schema names them.
typedef struct {
  uint8_t seq;        // high nibble, low nibble zero
  uint8_t counter;    // free-running 8-bit counter
  uint8_t enable_mask; // 0xFE = speaker only, 0xFF = microphone capture too
  bool speaker;       // 0x13 tag for the membrane, 0x16 for the jack

  const uint8_t *state; // 63-byte snapshot; NULL means "zeros"
  const uint8_t *haptics; // 64 bytes of int8 PCM; NULL means silence
  const uint8_t *opus;  // the Opus frame, `opus_len` bytes
  uint8_t opus_len;     // normally 200 (CBR)
} pw_0x36;

// Serializes a complete report and returns the number of bytes written, or 0
// if the buffer was too small. The output is exactly PW_WIRE_SIZE bytes.
static inline size_t pw_0x36_serialize(uint8_t *out, size_t cap, const pw_0x36 *r) {
  pw_writer w = pw_writer_new(out, cap);

  uint8_t haptic_lengths[5] = {64, 64, 64, 64, 64};

  pw_u8(&w, PW_WIRE_ID);
  pw_u8(&w, (uint8_t)((r->seq & 0x0F) << 4));

  // The config sub-packet is not a plain tagged payload: tag, length, the
  // enable mask, the five haptic lengths and the counter.
  pw_u8(&w, 0x11 | 0x80);
  pw_u8(&w, 7);
  pw_u8(&w, r->enable_mask);
  pw_bytes(&w, haptic_lengths, sizeof(haptic_lengths));
  pw_u8(&w, r->counter);

  pw_subpacket(&w, 0x10, r->state, PW_WIRE_STATE);
  pw_subpacket(&w, 0x12, r->haptics, PW_WIRE_HAPTIC);

  // The speaker sub-packet is always the full 200-byte Opus block, zero-padded
  // when the encoder produced less (which CBR never does).
  pw_u8(&w, (uint8_t)((r->speaker ? 0x13 : 0x16) | 0x80));
  pw_u8(&w, PW_WIRE_OPUS);
  size_t copy = r->opus ? (r->opus_len < PW_WIRE_OPUS ? r->opus_len : PW_WIRE_OPUS) : 0;
  if (copy) pw_bytes(&w, r->opus, copy);
  if (copy < PW_WIRE_OPUS) pw_zeros(&w, PW_WIRE_OPUS - copy);

  pw_zeros(&w, PW_WIRE_SIZE - PW_WIRE_CRC_BYTES - w.len);
  pw_crc32(&w);

  if (!pw_ok(&w) || w.len != PW_WIRE_SIZE) return 0;
  return w.len;
}

#endif /* PADWAVE_WIRE_H */
