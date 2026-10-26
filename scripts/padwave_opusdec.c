// padwave_opusdec — decode raw Opus packets to s16le PCM, 48 kHz mono.
//
// The DualSense's microphone sends bare Opus frames (no container, no Ogg),
// which is exactly what libopus wants. Elixir talks to this over a port with
// `packet: 4` framing:
//
//   in : 4-byte big-endian length, then the Opus packet
//   out: 4-byte big-endian length, then interleaved s16le PCM
//
// A long-lived port, not a process per packet: at ~100 frames a second the
// fork/exec cost would dominate. Each decoded frame is 480 samples (10 ms).
//
// Build: cc -O2 -o padwave_opusdec padwave_opusdec.c -lopus
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <opus/opus.h>

#define MAX_FRAME 5760 /* 120 ms at 48 kHz, the largest Opus frame */

static int read_exact(void *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    size_t n = fread((char *)buf + got, 1, len - got, stdin);
    if (n == 0) return 0;
    got += n;
  }
  return 1;
}

static int write_exact(const void *buf, size_t len) {
  size_t put = 0;
  while (put < len) {
    size_t n = fwrite((const char *)buf + put, 1, len - put, stdout);
    if (n == 0) return 0;
    put += n;
  }
  return fflush(stdout) == 0;
}

int main(void) {
  int error = 0;
  OpusDecoder *decoder = opus_decoder_create(48000, 1, &error);
  if (error != OPUS_OK || !decoder) {
    fprintf(stderr, "opus_decoder_create: %s\n", opus_strerror(error));
    return 1;
  }

  uint8_t header[4];
  int16_t pcm[MAX_FRAME];

  while (read_exact(header, 4)) {
    uint32_t len = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                   ((uint32_t)header[2] << 8) | (uint32_t)header[3];
    if (len == 0 || len > 8192) break;

    uint8_t *packet = malloc(len);
    if (!packet) break;
    if (!read_exact(packet, len)) {
      free(packet);
      break;
    }

    int samples = opus_decode(decoder, packet, (opus_int32)len, pcm, MAX_FRAME, 0);
    free(packet);

    if (samples < 0) {
      // A lost or corrupt packet still needs an answer, or the framing drifts:
      // Opus's own error concealment is what the decoder would do anyway.
      samples = opus_decode(decoder, NULL, 0, pcm, 480, 0);
      if (samples < 0) break;
    }

    uint32_t out_len = (uint32_t)samples * sizeof(int16_t);
    uint8_t out_header[4] = {
        (uint8_t)(out_len >> 24), (uint8_t)(out_len >> 16), (uint8_t)(out_len >> 8),
        (uint8_t)out_len};

    if (!write_exact(out_header, 4) || !write_exact(pcm, out_len)) break;
  }

  opus_decoder_destroy(decoder);
  return 0;
}
