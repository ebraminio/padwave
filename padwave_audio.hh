// padwave_audio.hh — PCM-facing codec wrappers.
//
// The rest of padwave passes 48 kHz PCM and never touches libopus. The Opus
// packet format, encoder settings and decoder live here.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <opus/opus.h>

namespace padwave {

struct AudioEncoder {
  ::OpusEncoder *encoder = nullptr;

  AudioEncoder() = default;

  ~AudioEncoder() { close(); }

  AudioEncoder(const AudioEncoder &) = delete;
  AudioEncoder &operator=(const AudioEncoder &) = delete;

  bool valid() const { return encoder != nullptr; }

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

struct AudioDecoder {
  ::OpusDecoder *decoder = nullptr;

  AudioDecoder() = default;

  ~AudioDecoder() { close(); }

  AudioDecoder(const AudioDecoder &) = delete;
  AudioDecoder &operator=(const AudioDecoder &) = delete;

  bool valid() const { return decoder != nullptr; }

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

} // namespace padwave
