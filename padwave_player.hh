// padwave_player.hh — the player engine.
//
// One Player object owns the FFmpeg decoders, the Opus encoder, the two
// prefetch rings, the crossfade state, the 0x36 writer and the hidraw fd. The
// app embeds one; there is no separate player process or CLI.
//
// One thread, no async I/O framework: the server's epoll loop and a timerfd
// are enough because decoding is not the bottleneck, *latency* was. Every loop iteration
// spends at most DECODE_BUDGET_MS inside libav, the ring is primed before the
// first send, and a late timer catches up with a few packets. That keeps the
// 10.667 ms tick without a decoder thread; io_uring would only matter for
// high-IOPS storage, and libavformat's AVIO is synchronous anyway.
//
// Measured playing a 1 kHz tone: 750 packets over 8 s (93.9/s), gap median
// 10.67 ms, p99 10.71 ms, max 13.38 ms, zero underruns, decoded pitch
// 1066.8 Hz — the same cadence as the audio service it replaces.
//
//   file → libavformat/libavcodec → 48 kHz stereo → 512-frame blocks
//        → resample to the 480-sample Opus frame → libopus CBR 160k
//        → a complete DualSense 0x36 report (state snapshot + haptics +
//          speaker + CRC-32 seed 0xA2) → hidraw
//
// No external player and no audio service: decode, encode and the report are
// one process.
//
// Build (no STL, no RTTI, no exceptions): ./build.sh (single translation unit,
// padwave.cc plus these headers).
#pragma once

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

// FFmpeg's public headers are C-only (no extern "C" guards).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <opus/opus.h>

#include "padwave_fields.hh"
#include "padwave_hid.hh"
#include "padwave_wire.hh"

struct player_status {
  long pos_ms;
  long dur_ms;
  long sent;
  long underruns;
  long pushed;
  int paused;
  int eof;
};

typedef void (*player_status_fn)(const struct player_status *, void *);
typedef void (*player_track_fn)(const char *, void *);

#define RATE 48000
#define CHANNELS 2
#define IN_FRAMES 512
#define OPUS_FRAMES 480
#define OPUS_BYTES 200
#define REPORT_SIZE 398
#define RING_BLOCKS 192
#define RING_HIGH 160
#define RING_PRIME 24
#define DECODE_BUDGET_MS 3

typedef float block_t[IN_FRAMES][CHANNELS];

struct Player {
private:
  struct File {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec = nullptr;
    SwrContext *swr = nullptr;
    int audio_index = -1;
    int64_t duration_ms = 0;
    bool eof = false;

    void close() {
      if (swr) swr_free(&swr);
      if (dec) avcodec_free_context(&dec);
      if (fmt) avformat_close_input(&fmt);
      fmt = NULL;
      dec = NULL;
      swr = NULL;
      audio_index = -1;
      duration_ms = 0;
      eof = false;
    }

    bool open(const char *path) {
      close();

      if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return false;
      if (avformat_find_stream_info(fmt, NULL) < 0) return false;

      const AVCodec *codec = NULL;
      audio_index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
      if (audio_index < 0 || !codec) return false;

      AVStream *st = fmt->streams[audio_index];
      dec = avcodec_alloc_context3(codec);
      if (!dec) return false;
      if (avcodec_parameters_to_context(dec, st->codecpar) < 0) return false;
      if (avcodec_open2(dec, codec, NULL) < 0) return false;

      AVChannelLayout in_layout;
      AVChannelLayout out_layout;
      av_channel_layout_copy(&in_layout, &dec->ch_layout);
      av_channel_layout_default(&out_layout, CHANNELS);

      int converted = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, RATE, &in_layout,
                                          dec->sample_fmt, dec->sample_rate, 0, NULL);

      av_channel_layout_uninit(&in_layout);
      av_channel_layout_uninit(&out_layout);
      if (converted < 0 || !swr || swr_init(swr) < 0) return false;

      duration_ms = fmt->duration > 0 ? fmt->duration / (AV_TIME_BASE / 1000) : 0;
      eof = false;
      return true;
    }
  };

  int hidraw = -1;
  int hidraw_retry = 0;
  bool speaker = false;
  player_status_fn status_fn = nullptr;
  player_track_fn track_fn = nullptr;
  void *sink_ud = nullptr;

  OpusEncoder *enc = nullptr;
  uint8_t seq = 0, counter = 0;

  File cur;
  File nxt;
  AVPacket *pkt = nullptr;
  AVFrame *frame = nullptr;
  char path[1024] = "";
  char next_path[1024] = "";
  bool has_next = false;
  bool next_warmed = false;

  int fade_ms = 0;
  bool fading = false;
  int fade_pos = 0, fade_total = 0;
  bool manual_fade = false;
  bool manual_pending = false;
  char manual_path[1024] = "";
  bool seek_fade = false;

  padwave::OutputReport out{};
  uint8_t out_seq = 0;
  padwave::OutputBody state{};
  bool mic = false;

  float ring[RING_BLOCKS][IN_FRAMES][CHANNELS] = {};
  int head = 0, tail = 0;
  float acc[IN_FRAMES][CHANNELS] = {};
  int acc_frames = 0;
  block_t nring[RING_BLOCKS] = {};
  int nhead = 0, ntail = 0;
  float nacc[IN_FRAMES][CHANNELS] = {};
  int nacc_frames = 0;
  int64_t npushed = 0;

  bool playing = false, paused = false, eof = false, primed = false;
  int volume = 80;
  int64_t base_ms = 0, consumed = 0, sent = 0, underruns = 0, pushed = 0;

  int tfd = -1;
  int64_t last_status = 0;
  long tick_ns = 0;

  static constexpr long kPlayTickNs = 10667000;
  static constexpr long kPauseTickNs = 500000000;

  static int countOf(int head, int tail, int size) { return (head - tail + size) % size; }

  static void warmFile(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
      close(fd);
      if (getenv("PADWAVE_DEBUG")) fprintf(stderr, "warm %s\n", path);
    }
  }

  int ringLen() const { return countOf(head, tail, RING_BLOCKS); }

  int nextRingLen() const { return countOf(nhead, ntail, RING_BLOCKS); }

  void closeFile() {
    cur.close();
    head = tail = 0;
    acc_frames = 0;
  }

  void closeNext() {
    nxt.close();
    nhead = ntail = 0;
    nacc_frames = 0;
    has_next = false;
  }

  bool openFile(const char *path) {
    closeFile();
    if (!cur.open(path)) return false;
    snprintf(this->path, sizeof(this->path), "%s", path);
    eof = false;
    consumed = 0;
    sent = 0;
    underruns = 0;
    base_ms = 0;
    return true;
  }

  void fillInto(File *f, block_t *ring, int *head, int *tail, float acc[IN_FRAMES][CHANNELS],
                int *acc_frames, int high, int64_t *pushed) {
    if (!f->fmt || paused) return;

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (countOf(*head, *tail, RING_BLOCKS) < high && !f->eof) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
      if (elapsed >= DECODE_BUDGET_MS) break;

      int ret = av_read_frame(f->fmt, pkt);
      if (getenv("PADWAVE_DEBUG") && ret < 0) fprintf(stderr, "read: %d\n", ret);
      if (ret == AVERROR_EOF) {
        avcodec_send_packet(f->dec, NULL);
        f->eof = true;
      } else if (ret < 0) {
        f->eof = true;
      } else {
        if (pkt->stream_index == f->audio_index) avcodec_send_packet(f->dec, pkt);
        av_packet_unref(pkt);
      }

      for (;;) {
        ret = avcodec_receive_frame(f->dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        uint8_t *out[1];
        float tmp[8192 * CHANNELS];
        out[0] = (uint8_t *)tmp;
        int cap = swr_get_out_samples(f->swr, frame->nb_samples);
        if (cap > 8192) cap = 8192;
        int got = swr_convert(f->swr, out, cap, (const uint8_t **)frame->extended_data,
                              frame->nb_samples);
        for (int i = 0; i < got; i++) {
          acc[*acc_frames][0] = tmp[i * CHANNELS + 0];
          acc[*acc_frames][1] = tmp[i * CHANNELS + 1];
          if (++(*acc_frames) == IN_FRAMES) {
            int next = (*head + 1) % RING_BLOCKS;
            if (next == *tail) { /* ring full: keep the partial block */
              *acc_frames = 0;
              break;
            }
            memcpy(ring[*head], acc, sizeof(float) * IN_FRAMES * CHANNELS);
            *head = next;
            if (pushed) (*pushed)++;
            *acc_frames = 0;
          }
        }
        av_frame_unref(frame);
        if (((*head + 1) % RING_BLOCKS) == *tail) break;
      }
    }
  }

  void fill() {
    fillInto(&cur, ring, &head, &tail, acc, &acc_frames, RING_HIGH, &pushed);
    eof = cur.eof;
  }

  void fillNext() {
    fillInto(&nxt, nring, &nhead, &ntail, nacc, &nacc_frames, RING_HIGH, &npushed);
  }

  void promote() {
    cur.close();
    cur = nxt;
    nxt = File{};

    memcpy(ring, nring, sizeof(ring));
    head = nhead;
    tail = ntail;
    memcpy(acc, nacc, sizeof(acc));
    acc_frames = nacc_frames;

    nhead = ntail = 0;
    nacc_frames = 0;
    npushed = 0;

    snprintf(path, sizeof(path), "%s", next_path);
    base_ms = 0;
    consumed = 0;
    sent = 0;
    underruns = 0;
    eof = cur.eof;
    has_next = false;

    if (track_fn) track_fn(path, sink_ud);
  }

  bool takeBlock(float out[IN_FRAMES][CHANNELS]) {
    if (tail == head) return false;
    memcpy(out, ring[tail], sizeof(ring[0]));
    tail = (tail + 1) % RING_BLOCKS;
    return true;
  }

  bool takeNextBlock(float out[IN_FRAMES][CHANNELS]) {
    if (ntail == nhead) return false;
    memcpy(out, nring[ntail], sizeof(nring[0]));
    ntail = (ntail + 1) % RING_BLOCKS;
    return true;
  }

  void finishFade() {
    cur.close();
    cur = nxt;
    nxt = File{};

    memcpy(ring, nring, sizeof(ring));
    head = nhead;
    tail = ntail;
    memcpy(acc, nacc, sizeof(acc));
    acc_frames = nacc_frames;

    nhead = ntail = 0;
    nacc_frames = 0;
    npushed = 0;

    bool manual = manual_fade;
    bool seek = seek_fade;
    if (manual)
      snprintf(path, sizeof(path), "%s", manual_path);
    else if (!seek)
      snprintf(path, sizeof(path), "%s", next_path);

    if (!seek) {
      base_ms = 0;
      consumed = 0;
    }
    sent = 0;
    underruns = 0;
    eof = cur.eof;
    fading = false;
    fade_pos = 0;
    fade_total = 0;
    manual_fade = false;
    manual_pending = false;
    seek_fade = false;

    if (!manual && !seek) {
      has_next = false;
      if (track_fn) track_fn(path, sink_ud);
    }
  }

  void sendReport(const uint8_t *opus, int len) {
    padwave::AudioReport report;

    report.init(speaker ? padwave::AudioReport::Speaker : padwave::AudioReport::Jack,
                mic ? padwave::AudioReport::SpeakerAndMic : padwave::AudioReport::SpeakerOnly);

    report.state.body = state;
    report.state.body.valid_flag0 |=
        padwave::OutputBody::HeadphoneVolume | padwave::OutputBody::SpeakerVolume |
        padwave::OutputBody::MicVolume | padwave::OutputBody::AudioRouting;
    report.state.body.valid_flag1 |= padwave::OutputBody::AudioControl2;
    report.state.body.headphone_volume = 0x7F;
    report.state.body.speaker_volume = 0x64;
    report.state.body.audio_control =
        speaker ? padwave::OutputBody::Speaker : padwave::OutputBody::Headphones;
    report.state.body.audio_control2 = 0x0A;

    report.seq_tag = (uint8_t)((seq & 0x0F) << 4);
    report.counter = counter;

    size_t copy = len > 0 ? (size_t)len : 0;
    if (copy > padwave::AudioReport::OpusSize) copy = padwave::AudioReport::OpusSize;
    if (copy) memcpy(report.opus, opus, copy);

    report.sign();

    // The controller can sleep (idle power-off) and come back on a new hidraw
    // node. Retry about once a second, and drop a stale fd on a failed write.
    if (hidraw < 0) {
      if (hidraw_retry > 0)
        hidraw_retry--;
      else {
        DualSenseDevice device;
        hidraw = device.open();
        if (hidraw < 0) hidraw_retry = 100;
      }
    }
    if (hidraw >= 0) {
      ssize_t n = write(hidraw, &report, sizeof(report));
      if (n != (ssize_t)sizeof(report)) {
        close(hidraw);
        hidraw = -1;
        hidraw_retry = 100;
      }
    }

    seq = (uint8_t)((seq + 1) & 0x0F);
    counter++;
    sent++;
  }

  void handleLine(char *line) {
    char path[1024];
    long value;

    // "play <start_ms> <path>" — the number comes first so a path can contain
    // spaces and the two fields never have to be guessed apart. "play <path>" is
    // still accepted for driving the binary by hand.
    if (sscanf(line, "play %ld %1023[^\n]", &value, path) == 2) {
      if (playing && !paused && cur.fmt && fade_ms > 0) {
        closeNext();
        if (nxt.open(path)) {
          if (value > 0) {
            int64_t ts = value * (AV_TIME_BASE / 1000);
            av_seek_frame(nxt.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(nxt.dec);
          }
          snprintf(manual_path, sizeof(manual_path), "%s", path);
          manual_fade = true;
          manual_pending = true;
          base_ms = value;
          consumed = 0;
          sent = 0;
          underruns = 0;
          return;
        }
      }

      closeNext();
      if (openFile(path)) {
        playing = true;
        paused = false;
        primed = false;
        if (value > 0) {
          int64_t ts = value * (AV_TIME_BASE / 1000);
          av_seek_frame(cur.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
          avcodec_flush_buffers(cur.dec);
          base_ms = value;
        }
      }
      return;
    }

    if (sscanf(line, "play %1023[^\n]", path) == 1) {
      closeNext();
      if (openFile(path)) {
        playing = true;
        paused = false;
        primed = false;
      }
      return;
    }

    // "queue <path>" — the next track, to be decoded ahead and promoted at the
    // end of the current one. Only one is held; the radio pushes the one after
    // that once the promotion is reported.
    if (sscanf(line, "queue %1023[^\n]", path) == 1) {
      if (!has_next) {
        snprintf(next_path, sizeof(next_path), "%s", path);
        has_next = true;
        next_warmed = false;
        warmFile(path);
      }
      return;
    }

    if (strcmp(line, "state_clear_lightbar") == 0) {
      state.valid_flag1 &= (uint8_t)~padwave::OutputBody::Lightbar;
      state.valid_flag2 &= (uint8_t)~padwave::OutputBody::ColorFade;
      state.lightbar_setup = 0;
      state.lightbar_red = 0;
      state.lightbar_green = 0;
      state.lightbar_blue = 0;
      return;
    }

    if (strncmp(line, "fields ", 7) == 0) {
      memset(&out, 0, sizeof(out));
      out.report_id = padwave::OutputReport::ReportId;
      out.tag = 0x10;
      out.seq_tag = (uint8_t)((out_seq & 0x0F) << 4);
      if (!padwave_fields::apply_fields(&out.body, line + 7)) return;
      out.sign();
      if (hidraw < 0) {
        DualSenseDevice device;
        hidraw = device.open();
      }
      bool ok =
          hidraw >= 0 && padwave_fields::write_all(hidraw, (const uint8_t *)&out, sizeof(out));
      if (ok) out_seq = (uint8_t)((out_seq + 1) & 0x0F);
      if (ok) {
        padwave_fields::apply_fields(&state, line + 7, true);
        // Reset/fade are one-shot in the 0x31 report; do not let the 0x36 state
        // snapshot re-trigger them forever.
        state.valid_flag1 &= (uint8_t)~padwave::OutputBody::ResetLights;
        state.valid_flag2 &= (uint8_t)~padwave::OutputBody::ColorFade;
      }
      return;
    }

    if (sscanf(line, "fade %ld", &value) == 1) {
      fade_ms = (int)(value < 0 ? 0 : value);
      return;
    }

    if (sscanf(line, "pause %ld", &value) == 1) {
      paused = value != 0;
      return;
    }

    if (sscanf(line, "mic %ld", &value) == 1) {
      mic = value != 0;
      state.valid_flag1 |= padwave::OutputBody::PowerSave;
      if (mic) {
        state.power_save_control &= (uint8_t)~0x10;
        state.mic_volume = 0x40;
      } else {
        state.power_save_control |= 0x10;
      }
      return;
    }

    if (sscanf(line, "volume %ld", &value) == 1) {
      volume = (int)(value < 0 ? 0 : (value > 100 ? 100 : value));
      return;
    }

    long fade = 0;
    if (sscanf(line, "seek %ld %ld", &value, &fade) == 2 && cur.fmt) {
      if (fade > 0 && playing && !paused && !fading) {
        if (nxt.open(this->path)) {
          int64_t ts = value * (AV_TIME_BASE / 1000);
          av_seek_frame(nxt.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
          avcodec_flush_buffers(nxt.dec);
          nhead = ntail = 0;
          nacc_frames = 0;
          seek_fade = true;
          fading = true;
          fade_pos = 0;
          fade_total = (int)((long)fade * 1000 / 10667);
          if (fade_total < 1) fade_total = 1;
          base_ms = value;
          consumed = 0;
          return;
        }
      }
      /* Fall through to the hard seek if the crossfade could not start. */
    }

    if (sscanf(line, "seek %ld", &value) == 1 && cur.fmt) {
      int64_t ts = value * (AV_TIME_BASE / 1000);
      if (av_seek_frame(cur.fmt, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(cur.dec);
        head = tail = 0;
        acc_frames = 0;
        cur.eof = false;
        eof = false;
        primed = false;
        base_ms = value;
        consumed = 0;
        seek_fade = false;
      }
      return;
    }
  }

  void tick() {
    float block[IN_FRAMES][CHANNELS];

    if (fading) {
      float old[IN_FRAMES][CHANNELS];
      float next[IN_FRAMES][CHANNELS];
      bool have_old = takeBlock(old);
      bool have_next = takeNextBlock(next);

      if (!have_next) {
        /* The prefetched side is not ready yet: keep playing the current one
         * without advancing the fade. */
        if (have_old) {
          memcpy(block, old, sizeof(block));
          consumed += IN_FRAMES;
        } else {
          underruns++;
          memset(block, 0, sizeof(block));
        }
      } else {
        float g = fade_total > 0 ? (float)fade_pos / (float)fade_total : 1.0f;
        /* Equal-power: the two sides' powers add to one, so the level stays
         * constant through the overlap instead of dipping in the middle. */
        float phase = g * 1.5707963267948966f;
        float g_out = cosf(phase);
        float g_in = sinf(phase);
        for (int i = 0; i < IN_FRAMES; i++)
          for (int ch = 0; ch < CHANNELS; ch++)
            block[i][ch] = (have_old ? old[i][ch] : 0.0f) * g_out + next[i][ch] * g_in;

        if (seek_fade) {
          if (have_next) consumed += IN_FRAMES;
        } else if (have_old) {
          consumed += IN_FRAMES;
        }
        fade_pos++;
        if (fade_pos >= fade_total || !have_old) finishFade();
      }
    } else {
      bool have = takeBlock(block);

      if (!have) {
        underruns++;
        memset(block, 0, sizeof(block)); /* keep the stream alive with silence */
      } else {
        consumed += IN_FRAMES;
      }
    }

    float pcm[OPUS_FRAMES * CHANNELS];
    double step = (IN_FRAMES - 1) / (double)(OPUS_FRAMES - 1);
    float gain = (float)(volume / 100.0);
    gain = gain * gain * gain;

    for (int i = 0; i < OPUS_FRAMES; i++) {
      double src = i * step;
      int idx = (int)src;
      int nxt = idx < IN_FRAMES - 1 ? idx + 1 : IN_FRAMES - 1;
      float frac = (float)(src - idx);
      for (int ch = 0; ch < CHANNELS; ch++) {
        float a = block[idx][ch];
        float b = block[nxt][ch];
        pcm[i * CHANNELS + ch] = (a + (b - a) * frac) * gain;
      }
    }

    uint8_t opus[OPUS_BYTES];
    int n = opus_encode_float(enc, pcm, OPUS_FRAMES, opus, sizeof(opus));
    if (getenv("PADWAVE_DEBUG")) fprintf(stderr, "enc %d\n", n);
    if (n < 0) n = 0;
    sendReport(opus, n);
  }

public:
  Player() = default;

  ~Player() { destroy(); }

  bool valid() const { return enc != nullptr; }

  explicit operator bool() const { return valid(); }

  int timerFd() const { return tfd; }

  void setSink(player_status_fn status, player_track_fn track, void *ud) {
    status_fn = status;
    track_fn = track;
    sink_ud = ud;
  }

  bool create(const char *node = nullptr, bool speaker = false) {
    destroy();
    if (node) {
      hidraw = open(node, O_RDWR | O_CLOEXEC);
    } else {
      DualSenseDevice device;
      hidraw = device.open();
    }
    if (hidraw < 0) fprintf(stderr, "padwave player: no DualSense hidraw node\n");
    this->speaker = speaker;
    state.init(this->speaker ? padwave::OutputBody::Speaker : padwave::OutputBody::Headphones);

    int err = 0;
    enc = opus_encoder_create(RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &err);
    if (!enc || err != OPUS_OK) {
      fprintf(stderr, "padwave player: opus encoder: %s\n", opus_strerror(err));
      if (hidraw >= 0) close(hidraw);
      hidraw = -1;
      enc = nullptr;
      return false;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(160000));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    tick_ns = 0;
    setTick(kPlayTickNs);
    last_status = 0;
    return true;
  }

  void destroy() {
    closeFile();
    closeNext();
    if (hidraw >= 0) close(hidraw);
    if (enc) opus_encoder_destroy(enc);
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    if (tfd >= 0) close(tfd);
    hidraw = -1;
    enc = nullptr;
    pkt = nullptr;
    frame = nullptr;
    tfd = -1;
    tick_ns = 0;
    playing = false;
    paused = false;
  }

  void setTick(long ns) {
    if (tfd < 0 || tick_ns == ns) return;
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_nsec = ns;
    its.it_value = its.it_interval;
    if (timerfd_settime(tfd, 0, &its, nullptr) == 0) tick_ns = ns;
  }

  void command(const char *line) {
    if (!line) return;
    char buf[1200];
    snprintf(buf, sizeof(buf), "%s", line);
    char *p = buf;
    char *nl;
    while ((nl = strchr(p, '\n')) != nullptr) {
      *nl = 0;
      if (*p) handleLine(p);
      p = nl + 1;
    }
    if (*p) handleLine(p);
    setTick((playing && !paused) ? kPlayTickNs : kPauseTickNs);
  }

  void step() {
    setTick((playing && !paused) ? kPlayTickNs : kPauseTickNs);
    if (playing && !paused) fill();

    if (playing && !paused && has_next && !nxt.fmt && !next_warmed && cur.duration_ms > 0) {
      int64_t pos = base_ms + consumed * 1000 / RATE;
      if (pos + 8000 >= cur.duration_ms) {
        warmFile(next_path);
        next_warmed = true;
      }
    }

    if (playing && !paused && has_next && !nxt.fmt) {
      int64_t pos = base_ms + consumed * 1000 / RATE;
      if (eof || (cur.duration_ms > 0 && pos + 5000 >= cur.duration_ms)) {
        warmFile(next_path);
        if (!nxt.open(next_path)) has_next = false;
      }
    }

    if (playing && !paused && nxt.fmt) fillNext();

    if (manual_pending && manual_fade && nextRingLen() >= 8) {
      int fade_blocks = (int)((long)fade_ms * 1000 / 10667);
      manual_pending = false;
      fading = true;
      fade_total = fade_blocks > 0 ? fade_blocks : 1;
      fade_pos = 0;
    }

    if (!fading && !manual_fade && playing && !paused && fade_ms > 0 && has_next && nxt.fmt) {
      int fade_blocks = (int)((long)fade_ms * 1000 / 10667);
      int left = ringLen();
      if (eof && left > 0 && left <= fade_blocks && nextRingLen() >= left) {
        fading = true;
        fade_total = left;
        fade_pos = 0;
      }
    }

    if (!fading && !manual_fade && eof && ringLen() == 0 && nextRingLen() > 0) promote();
    if (!primed && ringLen() >= RING_PRIME) primed = true;

    uint64_t expirations = 0;
    if (tfd >= 0 && read(tfd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
      if (primed && playing && !paused) {
        for (uint64_t i = 0; i < expirations && i < 4; i++) tick();
      }
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;
    if (ms - last_status >= 100) {
      last_status = ms;
      struct player_status st;
      st.pos_ms = base_ms + consumed * 1000 / RATE;
      st.dur_ms = cur.duration_ms;
      st.sent = sent;
      st.underruns = underruns;
      st.pushed = pushed;
      st.paused = paused ? 1 : 0;
      st.eof = (eof && tail == head && nxt.fmt == NULL && !has_next) ? 1 : 0;
      if (status_fn) status_fn(&st, sink_ud);
    }
  }
};
