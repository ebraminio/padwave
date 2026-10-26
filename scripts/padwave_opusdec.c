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

// -e: encode mode. Reads framed s16le stereo PCM (480 samples per channel =
// 1920 bytes) and writes framed Opus packets, CBR 200 bytes, matching the
// speaker block the controller expects.
static int encode_loop(void) {
  int error = 0;
  OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
  if (error != OPUS_OK || !enc) {
    fprintf(stderr, "opus_encoder_create: %s\n", opus_strerror(error));
    return 1;
  }
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(160000));
  // CBR, like the audio service: the 0x36 speaker block is a fixed 200 bytes,
  // and a VBR packet shorter than that would be zero-padded, leaving trailing
  // bytes the controller's decoder has to make sense of (audible clicks).
  opus_encoder_ctl(enc, OPUS_SET_VBR(0));
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));

  unsigned char out[200];
  int16_t pcm[480 * 2];
  uint8_t header[4];

  while (read_exact(header, 4)) {
    uint32_t len = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                   ((uint32_t)header[2] << 8) | (uint32_t)header[3];
    if (len != sizeof(pcm)) break;
    if (!read_exact(pcm, len)) break;

    if (getenv("PADWAVE_DEBUG")) {
      int16_t mx = 0;
      for (int i = 0; i < 480 * 2; i++) if (pcm[i] > mx || -pcm[i] > mx) mx = pcm[i] < 0 ? -pcm[i] : pcm[i];
      fprintf(stderr, "enc input peak %d\n", mx);
    }
    float fpcm[480 * 2];
    for (int i = 0; i < 480 * 2; i++) fpcm[i] = (float)pcm[i] / 32768.0f;

    int n = opus_encode_float(enc, fpcm, 480, out, sizeof(out));
    if (n < 0) break;

    uint8_t out_header[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    if (!write_exact(out_header, 4) || !write_exact(out, n)) break;
  }

  opus_encoder_destroy(enc);
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "-e") == 0) return encode_loop();

  // -s decodes stereo (the speaker stream); the microphone is mono.
  int channels = (argc > 1 && strcmp(argv[1], "-s") == 0) ? 2 : 1;

  int error = 0;
  OpusDecoder *decoder = opus_decoder_create(48000, channels, &error);
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
      if (getenv("PADWAVE_DEBUG")) fprintf(stderr, "decode error: %s (len %u)\n", opus_strerror(samples), len);
      // A lost or corrupt packet still needs an answer, or the framing drifts:
      // Opus's own error concealment is what the decoder would do anyway.
      samples = opus_decode(decoder, NULL, 0, pcm, 480, 0);
      if (samples < 0) break;
    }

    if (getenv("PADWAVE_DEBUG")) {
      int16_t mx = 0;
      for (int i = 0; i < samples; i++) if (pcm[i] > mx || -pcm[i] > mx) mx = pcm[i] < 0 ? -pcm[i] : pcm[i];
      fprintf(stderr, "dec peak %d samples %d\n", mx, samples);
    }
    uint32_t out_len = (uint32_t)samples * (uint32_t)channels * sizeof(int16_t);
    uint8_t out_header[4] = {
        (uint8_t)(out_len >> 24), (uint8_t)(out_len >> 16), (uint8_t)(out_len >> 8),
        (uint8_t)out_len};

    if (!write_exact(out_header, 4) || !write_exact(pcm, out_len)) break;
  }

  opus_decoder_destroy(decoder);
  return 0;
}
