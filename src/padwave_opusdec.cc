// padwave_opusdec.cc — the Opus helper, in C++ and on the schema's constants.
//
// The opusdec mode of the padwave binary:
//   padwave opusdec -e : 512 stereo frames in, resampled to 480, Opus CBR out
//   padwave opusdec    : Opus in, 480 samples out (mono; -s for stereo)
//
// Framing is a 4-byte big-endian length before each packet, both directions —
// padwave's private port protocol, not the controller's.
//
// The sizes are not repeated here: they come from padwave_schemas.hh, the one
// description of the protocol.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <opus/opus.h>
}

#include "padwave_schemas.hh"

namespace {

constexpr int kRate = padwave::MicFrame::SampleRate;
constexpr int kOpusFrames = padwave::AudioReport::OpusFrames;
constexpr int kInputFrames = padwave::AudioReport::InputBlock;
constexpr int kOpusSize = padwave::AudioReport::OpusSize;
constexpr int kChannels = padwave::AudioReport::Channels;
constexpr int kMicSamples = padwave::MicFrame::Samples;

bool read_exact(void *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    size_t n = fread((char *)buf + got, 1, len - got, stdin);
    if (n == 0) return false;
    got += n;
  }
  return true;
}

bool write_exact(const void *buf, size_t len) {
  size_t put = 0;
  while (put < len) {
    size_t n = fwrite((const char *)buf + put, 1, len - put, stdout);
    if (n == 0) return false;
    put += n;
  }
  return fflush(stdout) == 0;
}

uint32_t read_length() {
  uint8_t h[4];
  if (!read_exact(h, 4)) return 0;
  return ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
}

bool write_length(uint32_t len) {
  uint8_t h[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
  return write_exact(h, 4);
}

int encode_loop() {
  int error = 0;
  OpusEncoder *enc = opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_AUDIO, &error);
  if (error != OPUS_OK || !enc) {
    fprintf(stderr, "opus_encoder_create: %s\n", opus_strerror(error));
    return 1;
  }
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(kOpusSize * 8 * 100));
  // CBR like the audio service: a VBR packet shorter than the fixed 200-byte
  // block would be zero-padded, leaving trailing bytes the controller's
  // decoder has to make sense of (audible clicks).
  opus_encoder_ctl(enc, OPUS_SET_VBR(0));
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));

  int16_t in[kInputFrames * kChannels];
  float fpcm[kOpusFrames * kChannels];
  unsigned char out[kOpusSize];

  for (;;) {
    uint32_t len = read_length();
    if (len != sizeof(in)) break;
    if (!read_exact(in, len)) break;

    // 512 input frames -> 480 Opus samples, the same linear resampling the
    // audio service has always done.
    double step = (kInputFrames - 1) / (double)(kOpusFrames - 1);
    for (int i = 0; i < kOpusFrames; i++) {
      double src = i * step;
      int idx = (int)src;
      int nxt = idx < kInputFrames - 1 ? idx + 1 : kInputFrames - 1;
      float frac = (float)(src - idx);
      for (int ch = 0; ch < kChannels; ch++) {
        float a = (float)in[idx * kChannels + ch] / 32768.0f;
        float b = (float)in[nxt * kChannels + ch] / 32768.0f;
        fpcm[i * kChannels + ch] = a + (b - a) * frac;
      }
    }

    int n = opus_encode_float(enc, fpcm, kOpusFrames, out, sizeof(out));
    if (n < 0) break;
    if (!write_length((uint32_t)n) || !write_exact(out, (size_t)n)) break;
  }

  opus_encoder_destroy(enc);
  return 0;
}

int decode_loop(bool stereo) {
  int channels = stereo ? 2 : 1;
  int error = 0;
  OpusDecoder *dec = opus_decoder_create(kRate, channels, &error);
  if (error != OPUS_OK || !dec) {
    fprintf(stderr, "opus_decoder_create: %s\n", opus_strerror(error));
    return 1;
  }

  int16_t pcm[5760]; // 120 ms at 48 kHz, the largest Opus frame
  for (;;) {
    uint32_t len = read_length();
    if (len == 0 || len > 8192) break;

    unsigned char packet[8192];
    if (!read_exact(packet, len)) break;

    // frame_size must be the packet's own sample count: passing the buffer size
    // instead makes libopus return a truncated, attenuated frame.
    int ns = opus_packet_get_nb_samples(packet, (opus_int32)len, kRate);
    int samples = opus_decode(dec, packet, (opus_int32)len, pcm, ns > 0 ? ns : kMicSamples, 0);

    if (samples < 0) {
      if (getenv("PADWAVE_DEBUG"))
        fprintf(stderr, "decode error: %s (len %u)\n", opus_strerror(samples), len);
      samples = opus_decode(dec, NULL, 0, pcm, kMicSamples, 0);
      if (samples < 0) break;
    }

    uint32_t out_len = (uint32_t)samples * (uint32_t)channels * sizeof(int16_t);
    if (!write_length(out_len) || !write_exact(pcm, out_len)) break;
  }

  opus_decoder_destroy(dec);
  return 0;
}

} // namespace

int opusdec_main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "-e") == 0) return encode_loop();

  bool stereo = argc > 1 && strcmp(argv[1], "-s") == 0;
  return decode_loop(stereo);
}
