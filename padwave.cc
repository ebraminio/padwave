// padwave — one translation unit: the player, the input decoder, the app and
// the web server. The wire formats live in padwave_wire.hh.
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

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

#include "padwave_wire.hh"
#include "padwave_app.hh"
#include "padwave_server.hh"

// Shared helpers for the native modes: finding the controller's hidraw node
// and writing a whole buffer to it.

// The sysfs uevent carries HID_ID=0005:0000054C:00000CE6 for the DualSense and
// ...0DF2 for the Edge. Scan a bounded range; each node's uevent is small.
static const char *const kDualsenseIds[] = {"054C:00000CE6", "054C:00000DF2"};

static inline int find_dualsense(char *node, size_t len) {
  for (int i = 0; i < 16; i++) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/hidraw/hidraw%d/device/uevent", i);
    FILE *f = fopen(path, "r");
    if (!f) continue;

    char line[256];
    bool match = false;
    while (fgets(line, sizeof(line), f)) {
      for (const char *id : kDualsenseIds) {
        if (strstr(line, id)) {
          match = true;
          break;
        }
      }
    }
    fclose(f);

    if (!match) continue;
    snprintf(node, len, "/dev/hidraw%d", i);
    return i;
  }
  return -1;
}

// Open the first DualSense. The player needs read/write; callers that only
// want to read should use find_dualsense(node, len) and open it themselves.
static inline int find_dualsense() {
  char node[64];
  if (find_dualsense(node, sizeof(node)) < 0) return -1;
  return open(node, O_RDWR | O_CLOEXEC);
}

static inline bool write_all(int fd, const void *buf, size_t len) {
  const uint8_t *data = (const uint8_t *)buf;
  size_t put = 0;
  while (put < len) {
    ssize_t n = write(fd, data + put, len - put);
    if (n <= 0) return false;
    put += (size_t)n;
  }
  return true;
}

// Output-body field application, shared by the player's 0x31 writer.
//
// Names are the OutputBody members; a flag word's value is a comma list of
// ValidFlag0/1/2 enumerator names; the two trigger parameter blocks are hex.
// Kept out of padwave_output.cc so the single player process can build both
// the 0x36 audio reports and these 0x31 control reports from the same schema.

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

// The tiny status bus from the native processes to the web front.
//
// Unix datagrams, one line each, fixed field order so the receiver can use
// sscanf. Sending is fire-and-forget: when the front is not running the
// sendto() is dropped and nothing else changes.
//
//   player <pos_ms> <dur_ms> <sent> <underruns> <paused>
//   input  <lx> <ly> <rx> <ry> <l2> <r2> <buttons> <battery> <jack>

// padwave_native — the whole playback path in one C program.
//
// One thread, no async I/O framework: poll() on stdin and a timerfd is enough
// because decoding is not the bottleneck, *latency* was. Every loop iteration
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
// Usage:  padwave native [hidraw-node]
//         (without a node it scans /sys/class/hidraw for a DualSense)
//
// stdin commands, one per line:
//   play <path> [start_ms]   pause <0|1>   volume <0..100>   seek <ms>   quit
// stdout, ten times a second:
//   pos <ms>   dur <ms>   sent <n>   underruns <n>   eof
//
// Build (no STL, no RTTI, no exceptions):
//   cc -std=c++23 -fno-exceptions -fno-rtti -fno-threadsafe-statics -O2
//      -o padwave padwave.cc padwave_native.cc padwave_opusdec.cc
//      $(pkg-config --cflags --libs libavformat libavcodec libswresample libavutil)
//      -lopus -lz -lm

// FFmpeg's public headers are C-only (no extern "C" guards), so a C++ caller
// has to provide the linkage wrapper itself.

#define RATE 48000
#define CHANNELS 2
#define IN_FRAMES 512   /* 512 samples per input block, 93.75 blocks/s */
#define OPUS_FRAMES 480 /* what the controller's decoder consumes */
#define OPUS_BYTES 200  /* CBR 160 kbps at 10 ms */
#define REPORT_SIZE 398
#define RING_BLOCKS 192    /* about two seconds: enough for a 1.5 s crossfade */
#define RING_HIGH 160      /* stop decoding above this many buffered blocks */
#define RING_PRIME 24      /* start sending once this many are buffered */
#define DECODE_BUDGET_MS 3 /* but never spend longer than this per iteration */

struct file {
  AVFormatContext *fmt;
  AVCodecContext *dec;
  struct SwrContext *swr;
  int audio_index;
  int64_t duration_ms;
  bool eof;
};

typedef float block_t[IN_FRAMES][CHANNELS];

struct player {
  int hidraw;
  int hidraw_retry;
  bool speaker; /* 0x13 tag instead of 0x16 */
  player_status_fn status_fn;
  player_track_fn track_fn;
  void *sink_ud;

  OpusEncoder *enc;
  uint8_t seq, counter;

  struct file cur; /* the station playing now */
  struct file nxt; /* the prefetched one, if any */
  AVPacket *pkt;
  AVFrame *frame;
  char path[1024];
  char next_path[1024];
  bool has_next;
  bool next_warmed;

  /* Crossfade at a station change: the last fade_total blocks of the current
   * station are mixed with the first fade_total blocks of the next one. */
  int fade_ms;
  bool fading;
  int fade_pos, fade_total;

  /* A manual station change (next/prev) fades too: the new station is opened
   * into the prefetch slot and the tick blends it in, without reporting a
   * track (the radio already made that one current). */
  bool manual_fade;
  bool manual_pending;
  char manual_path[1024];

  /* A seek inside the file can fade too: a second decoder opens the target
   * position and the tick blends into it, same as a station change. */
  bool seek_fade;

  /* The 0x31 control writer shares this process (and this hidraw fd) with the
   * 0x36 stream: one server for everything. */
  padwave::OutputReport out;
  uint8_t out_seq;

  /* The 0x31 features the writer has claimed, kept so the 0x36 state snapshot
   * carries them. The controller applies that snapshot 93 times a second; if it
   * only said "audio", the rumble and trigger values would be zeroed by the
   * stream. */
  padwave::OutputBody state;

  /* Experimental: the microphone is off in the 0x36 enable mask unless the UI
   * asks for the input level. The controller only sends 0xD4 mic frames while
   * this is on. */
  bool mic;

  float ring[RING_BLOCKS][IN_FRAMES][CHANNELS];
  int head, tail; /* head = write, tail = read */
  float acc[IN_FRAMES][CHANNELS];
  int acc_frames;

  /* Back-to-back playback: the next station is decoded into its own ring, then
   * swapped in when the current one drains. */
  block_t nring[RING_BLOCKS];
  int nhead, ntail;
  float nacc[IN_FRAMES][CHANNELS];
  int nacc_frames;
  int64_t npushed;

  bool playing, paused, eof, primed;
  int volume;
  int64_t base_ms;  /* source time at the start of the ring */
  int64_t consumed; /* input frames taken out of the ring */
  int64_t sent, underruns, pushed;
};

static volatile sig_atomic_t running = 1;

static void on_signal(int sig) {
  (void)sig;
  running = 0;
}

/* ---- 0x36 report -------------------------------------------------------- */

static void send_report(struct player *pl, const uint8_t *opus, int len) {
  padwave::AudioReport report;

  report.init(pl->speaker ? padwave::AudioReport::Speaker : padwave::AudioReport::Jack,
              pl->mic ? padwave::AudioReport::SpeakerAndMic : padwave::AudioReport::SpeakerOnly);

  report.state.body = pl->state;
  report.state.body.valid_flag0 |=
      padwave::OutputBody::HeadphoneVolume | padwave::OutputBody::SpeakerVolume |
      padwave::OutputBody::MicVolume | padwave::OutputBody::AudioRouting;
  report.state.body.valid_flag1 |= padwave::OutputBody::AudioControl2;
  report.state.body.headphone_volume = 0x7F;
  report.state.body.speaker_volume = 0x64;
  report.state.body.audio_control =
      pl->speaker ? padwave::OutputBody::Speaker : padwave::OutputBody::Headphones;
  report.state.body.audio_control2 = 0x0A;

  report.seq_tag = (uint8_t)((pl->seq & 0x0F) << 4);
  report.counter = pl->counter;

  size_t copy = len > 0 ? (size_t)len : 0;
  if (copy > padwave::AudioReport::OpusSize) copy = padwave::AudioReport::OpusSize;
  if (copy) memcpy(report.opus, opus, copy);

  report.sign();

  // The controller can sleep (idle power-off) and come back on a new hidraw
  // node. Retry about once a second, and drop a stale fd on a failed write.
  if (pl->hidraw < 0) {
    if (pl->hidraw_retry > 0)
      pl->hidraw_retry--;
    else {
      pl->hidraw = find_dualsense();
      if (pl->hidraw < 0) pl->hidraw_retry = 100;
    }
  }
  if (pl->hidraw >= 0) {
    ssize_t n = write(pl->hidraw, &report, sizeof(report));
    if (n != (ssize_t)sizeof(report)) {
      close(pl->hidraw);
      pl->hidraw = -1;
      pl->hidraw_retry = 100;
    }
  }

  pl->seq = (uint8_t)((pl->seq + 1) & 0x0F);
  pl->counter++;
  pl->sent++;
}

/* ---- decode ------------------------------------------------------------- */

static int count_of(int head, int tail, int size) { return (head - tail + size) % size; }

/* Ask the kernel to read a file ahead of time. Non-blocking: the read happens
 * on the kernel's schedule, so the later open() finds the pages in cache. */
static void warm_file(const char *path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
    close(fd);
    if (getenv("PADWAVE_DEBUG")) fprintf(stderr, "warm %s\n", path);
  }
}

static void file_close(struct file *f) {
  if (f->swr) swr_free(&f->swr);
  if (f->dec) avcodec_free_context(&f->dec);
  if (f->fmt) avformat_close_input(&f->fmt);
  f->fmt = NULL;
  f->dec = NULL;
  f->swr = NULL;
  f->audio_index = -1;
  f->duration_ms = 0;
  f->eof = false;
}

static bool file_open(struct file *f, const char *path) {
  file_close(f);

  if (avformat_open_input(&f->fmt, path, NULL, NULL) < 0) return false;
  if (avformat_find_stream_info(f->fmt, NULL) < 0) return false;

  const AVCodec *codec = NULL;
  f->audio_index = av_find_best_stream(f->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (f->audio_index < 0 || !codec) return false;

  AVStream *st = f->fmt->streams[f->audio_index];
  f->dec = avcodec_alloc_context3(codec);
  if (!f->dec) return false;
  if (avcodec_parameters_to_context(f->dec, st->codecpar) < 0) return false;
  if (avcodec_open2(f->dec, codec, NULL) < 0) return false;

  AVChannelLayout in_layout;
  AVChannelLayout out_layout;
  av_channel_layout_copy(&in_layout, &f->dec->ch_layout);
  av_channel_layout_default(&out_layout, CHANNELS);

  int converted = swr_alloc_set_opts2(&f->swr, &out_layout, AV_SAMPLE_FMT_FLT, RATE, &in_layout,
                                      f->dec->sample_fmt, f->dec->sample_rate, 0, NULL);

  av_channel_layout_uninit(&in_layout);
  av_channel_layout_uninit(&out_layout);
  if (converted < 0 || !f->swr || swr_init(f->swr) < 0) return false;

  f->duration_ms = f->fmt->duration > 0 ? f->fmt->duration / (AV_TIME_BASE / 1000) : 0;
  f->eof = false;
  return true;
}

static int ring_len(struct player *pl) { return count_of(pl->head, pl->tail, RING_BLOCKS); }
static int next_ring_len(struct player *pl) { return count_of(pl->nhead, pl->ntail, RING_BLOCKS); }

static void close_file(struct player *pl) {
  file_close(&pl->cur);
  pl->head = pl->tail = 0;
  pl->acc_frames = 0;
}

static void close_next(struct player *pl) {
  file_close(&pl->nxt);
  pl->nhead = pl->ntail = 0;
  pl->nacc_frames = 0;
  pl->has_next = false;
}

static bool open_file(struct player *pl, const char *path) {
  close_file(pl);
  if (!file_open(&pl->cur, path)) return false;
  snprintf(pl->path, sizeof(pl->path), "%s", path);
  pl->eof = false;
  pl->consumed = 0;
  pl->sent = 0;
  pl->underruns = 0;
  pl->base_ms = 0;
  return true;
}

/* Decode at most DECODE_BUDGET_MS worth of work into `ring`. This is the whole
 * trick: the loop keeps servicing the 10.667 ms timer because no single
 * iteration can sit inside libav for long. It serves the current station and
 * the prefetched one equally. */
static void fill_into(struct player *pl, struct file *f, block_t *ring, int *head, int *tail,
                      float acc[IN_FRAMES][CHANNELS], int *acc_frames, int high, int64_t *pushed) {
  if (!f->fmt || pl->paused) return;

  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (count_of(*head, *tail, RING_BLOCKS) < high && !f->eof) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    if (elapsed >= DECODE_BUDGET_MS) break;

    int ret = av_read_frame(f->fmt, pl->pkt);
    if (getenv("PADWAVE_DEBUG") && ret < 0) fprintf(stderr, "read: %d\n", ret);
    if (ret == AVERROR_EOF) {
      avcodec_send_packet(f->dec, NULL);
      f->eof = true;
    } else if (ret < 0) {
      f->eof = true;
    } else {
      if (pl->pkt->stream_index == f->audio_index) avcodec_send_packet(f->dec, pl->pkt);
      av_packet_unref(pl->pkt);
    }

    for (;;) {
      ret = avcodec_receive_frame(f->dec, pl->frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
      if (ret < 0) break;

      uint8_t *out[1];
      float tmp[8192 * CHANNELS];
      out[0] = (uint8_t *)tmp;
      int cap = swr_get_out_samples(f->swr, pl->frame->nb_samples);
      if (cap > 8192) cap = 8192;
      int got = swr_convert(f->swr, out, cap, (const uint8_t **)pl->frame->extended_data,
                            pl->frame->nb_samples);
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
      av_frame_unref(pl->frame);
      if (((*head + 1) % RING_BLOCKS) == *tail) break;
    }
  }
}

static void fill(struct player *pl) {
  fill_into(pl, &pl->cur, pl->ring, &pl->head, &pl->tail, pl->acc, &pl->acc_frames, RING_HIGH,
            &pl->pushed);
  pl->eof = pl->cur.eof;
}

static void fill_next(struct player *pl) {
  fill_into(pl, &pl->nxt, pl->nring, &pl->nhead, &pl->ntail, pl->nacc, &pl->nacc_frames, RING_HIGH,
            &pl->npushed);
}

/* The current station is exhausted; the prefetched one becomes current without
 * touching the encoder or the timer, which is what makes it gapless. */
static void promote(struct player *pl) {
  file_close(&pl->cur);
  pl->cur = pl->nxt;
  memset(&pl->nxt, 0, sizeof(pl->nxt));
  pl->nxt.audio_index = -1;

  memcpy(pl->ring, pl->nring, sizeof(pl->ring));
  pl->head = pl->nhead;
  pl->tail = pl->ntail;
  memcpy(pl->acc, pl->nacc, sizeof(pl->acc));
  pl->acc_frames = pl->nacc_frames;

  pl->nhead = pl->ntail = 0;
  pl->nacc_frames = 0;
  pl->npushed = 0;

  snprintf(pl->path, sizeof(pl->path), "%s", pl->next_path);
  pl->base_ms = 0;
  pl->consumed = 0;
  pl->sent = 0;
  pl->underruns = 0;
  pl->eof = pl->cur.eof;
  pl->has_next = false;

  if (pl->track_fn) pl->track_fn(pl->path, pl->sink_ud);
  fflush(stdout);
}

/* Take one 512-frame block out of the ring; returns false on underrun. */
static bool take_block(struct player *pl, float out[IN_FRAMES][CHANNELS]) {
  if (pl->tail == pl->head) return false;
  memcpy(out, pl->ring[pl->tail], sizeof(pl->ring[0]));
  pl->tail = (pl->tail + 1) % RING_BLOCKS;
  return true;
}

static bool take_next_block(struct player *pl, float out[IN_FRAMES][CHANNELS]) {
  if (pl->ntail == pl->nhead) return false;
  memcpy(out, pl->nring[pl->ntail], sizeof(pl->nring[0]));
  pl->ntail = (pl->ntail + 1) % RING_BLOCKS;
  return true;
}

/* The fade is over: the prefetched station becomes the current one. The blocks
 * already mixed are gone from nring, so the new ring continues exactly after
 * the faded-in part. */
static void finish_fade(struct player *pl) {
  file_close(&pl->cur);
  pl->cur = pl->nxt;
  memset(&pl->nxt, 0, sizeof(pl->nxt));
  pl->nxt.audio_index = -1;

  memcpy(pl->ring, pl->nring, sizeof(pl->ring));
  pl->head = pl->nhead;
  pl->tail = pl->ntail;
  memcpy(pl->acc, pl->nacc, sizeof(pl->acc));
  pl->acc_frames = pl->nacc_frames;

  pl->nhead = pl->ntail = 0;
  pl->nacc_frames = 0;
  pl->npushed = 0;

  bool manual = pl->manual_fade;
  bool seek = pl->seek_fade;
  if (manual)
    snprintf(pl->path, sizeof(pl->path), "%s", pl->manual_path);
  else if (!seek)
    snprintf(pl->path, sizeof(pl->path), "%s", pl->next_path);

  if (!seek) {
    pl->base_ms = 0;
    pl->consumed = 0;
  }
  pl->sent = 0;
  pl->underruns = 0;
  pl->eof = pl->cur.eof;
  pl->fading = false;
  pl->fade_pos = 0;
  pl->fade_total = 0;
  pl->manual_fade = false;
  pl->manual_pending = false;
  pl->seek_fade = false;

  if (!manual && !seek) {
    pl->has_next = false;
    if (pl->track_fn) pl->track_fn(pl->path, pl->sink_ud);
    fflush(stdout);
  }
}

/* ---- the tick ----------------------------------------------------------- */

static void tick(struct player *pl) {
  float block[IN_FRAMES][CHANNELS];

  if (pl->fading) {
    float old[IN_FRAMES][CHANNELS];
    float next[IN_FRAMES][CHANNELS];
    bool have_old = take_block(pl, old);
    bool have_next = take_next_block(pl, next);

    if (!have_next) {
      /* The prefetched side is not ready yet: keep playing the current one
       * without advancing the fade. */
      if (have_old) {
        memcpy(block, old, sizeof(block));
        pl->consumed += IN_FRAMES;
      } else {
        pl->underruns++;
        memset(block, 0, sizeof(block));
      }
    } else {
      float g = pl->fade_total > 0 ? (float)pl->fade_pos / (float)pl->fade_total : 1.0f;
      /* Equal-power: the two sides' powers add to one, so the level stays
       * constant through the overlap instead of dipping in the middle. */
      float phase = g * 1.5707963267948966f;
      float g_out = cosf(phase);
      float g_in = sinf(phase);
      for (int i = 0; i < IN_FRAMES; i++)
        for (int ch = 0; ch < CHANNELS; ch++)
          block[i][ch] = (have_old ? old[i][ch] : 0.0f) * g_out + next[i][ch] * g_in;

      if (pl->seek_fade) {
        if (have_next) pl->consumed += IN_FRAMES;
      } else if (have_old) {
        pl->consumed += IN_FRAMES;
      }
      pl->fade_pos++;
      if (pl->fade_pos >= pl->fade_total || !have_old) finish_fade(pl);
    }
  } else {
    bool have = take_block(pl, block);

    if (!have) {
      pl->underruns++;
      memset(block, 0, sizeof(block)); /* keep the stream alive with silence */
    } else {
      pl->consumed += IN_FRAMES;
    }
  }

  float pcm[OPUS_FRAMES * CHANNELS];
  double step = (IN_FRAMES - 1) / (double)(OPUS_FRAMES - 1);
  float gain = (float)(pl->volume / 100.0);
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
  int n = opus_encode_float(pl->enc, pcm, OPUS_FRAMES, opus, sizeof(opus));
  if (getenv("PADWAVE_DEBUG")) fprintf(stderr, "enc %d\n", n);
  if (n < 0) n = 0;
  send_report(pl, opus, n);
}

/* ---- commands ----------------------------------------------------------- */

static void handle_line(struct player *pl, char *line) {
  char path[1024];
  long value;

  // "play <start_ms> <path>" — the number comes first so a path can contain
  // spaces and the two fields never have to be guessed apart. "play <path>" is
  // still accepted for driving the binary by hand.
  if (sscanf(line, "play %ld %1023[^\n]", &value, path) == 2) {
    if (pl->playing && !pl->paused && pl->cur.fmt && pl->fade_ms > 0) {
      close_next(pl);
      if (file_open(&pl->nxt, path)) {
        if (value > 0) {
          int64_t ts = value * (AV_TIME_BASE / 1000);
          av_seek_frame(pl->nxt.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
          avcodec_flush_buffers(pl->nxt.dec);
        }
        snprintf(pl->manual_path, sizeof(pl->manual_path), "%s", path);
        pl->manual_fade = true;
        pl->manual_pending = true;
        pl->base_ms = value;
        pl->consumed = 0;
        pl->sent = 0;
        pl->underruns = 0;
        return;
      }
    }

    close_next(pl);
    if (open_file(pl, path)) {
      pl->playing = true;
      pl->paused = false;
      pl->primed = false;
      if (value > 0) {
        int64_t ts = value * (AV_TIME_BASE / 1000);
        av_seek_frame(pl->cur.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(pl->cur.dec);
        pl->base_ms = value;
      }
    }
    return;
  }

  if (sscanf(line, "play %1023[^\n]", path) == 1) {
    close_next(pl);
    if (open_file(pl, path)) {
      pl->playing = true;
      pl->paused = false;
      pl->primed = false;
    }
    return;
  }

  // "queue <path>" — the next station, to be decoded ahead and promoted at the
  // end of the current one. Only one is held; the radio pushes the one after
  // that once the promotion is reported.
  if (sscanf(line, "queue %1023[^\n]", path) == 1) {
    if (!pl->has_next) {
      snprintf(pl->next_path, sizeof(pl->next_path), "%s", path);
      pl->has_next = true;
      pl->next_warmed = false;
      warm_file(path);
    }
    return;
  }

  if (strncmp(line, "fields ", 7) == 0) {
    memset(&pl->out, 0, sizeof(pl->out));
    pl->out.report_id = padwave::OutputReport::ReportId;
    pl->out.tag = 0x10;
    pl->out.seq_tag = (uint8_t)((pl->out_seq & 0x0F) << 4);
    if (!padwave_fields::apply_fields(&pl->out.body, line + 7)) {
      printf("err field\n");
      fflush(stdout);
      return;
    }
    pl->out.sign();
    if (pl->hidraw < 0) pl->hidraw = find_dualsense();
    bool ok = pl->hidraw >= 0 && write_all(pl->hidraw, &pl->out, sizeof(pl->out));
    if (ok) pl->out_seq = (uint8_t)((pl->out_seq + 1) & 0x0F);
    if (ok) padwave_fields::apply_fields(&pl->state, line + 7, true);
    printf(ok ? "ok 1\n" : "err write\n");
    fflush(stdout);
    return;
  }

  if (sscanf(line, "fade %ld", &value) == 1) {
    pl->fade_ms = (int)(value < 0 ? 0 : value);
    return;
  }

  if (sscanf(line, "pause %ld", &value) == 1) {
    pl->paused = value != 0;
    return;
  }

  if (sscanf(line, "mic %ld", &value) == 1) {
    pl->mic = value != 0;
    pl->state.valid_flag1 |= padwave::OutputBody::PowerSave;
    if (pl->mic) {
      pl->state.power_save_control &= (uint8_t)~0x10;
      pl->state.mic_volume = 0x40;
    } else {
      pl->state.power_save_control |= 0x10;
    }
    return;
  }

  if (sscanf(line, "volume %ld", &value) == 1) {
    pl->volume = (int)(value < 0 ? 0 : (value > 100 ? 100 : value));
    return;
  }

  long fade = 0;
  if (sscanf(line, "seek %ld %ld", &value, &fade) == 2 && pl->cur.fmt) {
    if (fade > 0 && pl->playing && !pl->paused && !pl->fading) {
      if (file_open(&pl->nxt, pl->path)) {
        int64_t ts = value * (AV_TIME_BASE / 1000);
        av_seek_frame(pl->nxt.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(pl->nxt.dec);
        pl->nhead = pl->ntail = 0;
        pl->nacc_frames = 0;
        pl->seek_fade = true;
        pl->fading = true;
        pl->fade_pos = 0;
        pl->fade_total = (int)((long)fade * 1000 / 10667);
        if (pl->fade_total < 1) pl->fade_total = 1;
        pl->base_ms = value;
        pl->consumed = 0;
        return;
      }
    }
    /* Fall through to the hard seek if the crossfade could not start. */
  }

  if (sscanf(line, "seek %ld", &value) == 1 && pl->cur.fmt) {
    int64_t ts = value * (AV_TIME_BASE / 1000);
    if (av_seek_frame(pl->cur.fmt, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
      avcodec_flush_buffers(pl->cur.dec);
      pl->head = pl->tail = 0;
      pl->acc_frames = 0;
      pl->cur.eof = false;
      pl->eof = false;
      pl->primed = false;
      pl->base_ms = value;
      pl->consumed = 0;
      pl->seek_fade = false;
    }
    return;
  }

  if (strncmp(line, "quit", 4) == 0) running = 0;
}

/* ---- engine API --------------------------------------------------------- */

struct player_handle {
  struct player pl;
  int tfd;
  int64_t last_status;
};

void player_set_sink(void *handle, player_status_fn status, player_track_fn track, void *ud) {
  player_handle *h = (player_handle *)handle;
  if (!h) return;
  h->pl.status_fn = status;
  h->pl.track_fn = track;
  h->pl.sink_ud = ud;
}

void *player_create(const char *node, bool speaker) {
  player_handle *h = (player_handle *)calloc(1, sizeof(*h));
  if (!h) return nullptr;

  struct player &pl = h->pl;
  pl.volume = 80;
  pl.hidraw = node ? open(node, O_RDWR | O_CLOEXEC) : find_dualsense();
  if (pl.hidraw < 0) fprintf(stderr, "padwave_native: no DualSense hidraw node\n");
  pl.speaker = speaker;
  pl.state.init(pl.speaker ? padwave::OutputBody::Speaker : padwave::OutputBody::Headphones);

  int err = 0;
  pl.enc = opus_encoder_create(RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &err);
  if (!pl.enc || err != OPUS_OK) {
    fprintf(stderr, "padwave_native: opus encoder: %s\n", opus_strerror(err));
    free(h);
    return nullptr;
  }
  opus_encoder_ctl(pl.enc, OPUS_SET_BITRATE(160000));
  opus_encoder_ctl(pl.enc, OPUS_SET_VBR(0));
  opus_encoder_ctl(pl.enc, OPUS_SET_COMPLEXITY(0));

  pl.pkt = av_packet_alloc();
  pl.frame = av_frame_alloc();

  h->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  struct itimerspec its;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 10667000; /* 10.667 ms: 93.75 blocks/s */
  its.it_value = its.it_interval;
  if (h->tfd >= 0) timerfd_settime(h->tfd, 0, &its, NULL);

  h->last_status = 0;
  return h;
}

int player_timer_fd(void *handle) { return handle ? ((player_handle *)handle)->tfd : -1; }

void player_command(void *handle, const char *line) {
  player_handle *h = (player_handle *)handle;
  if (!h || !line) return;

  char buf[1200];
  snprintf(buf, sizeof(buf), "%s", line);

  char *p = buf;
  char *nl;
  while ((nl = strchr(p, '\n')) != nullptr) {
    *nl = 0;
    if (*p) handle_line(&h->pl, p);
    p = nl + 1;
  }
  if (*p) handle_line(&h->pl, p);
}

void player_step(void *handle) {
  player_handle *h = (player_handle *)handle;
  if (!h) return;
  struct player &pl = h->pl;

  if (pl.playing && !pl.paused) fill(&pl);

  // Warm the next file again shortly before it is needed: the queue-time
  // readahead can be stale by the end of a long track, and the open() at the
  // prefetch point must not read cold pages while the stream is running.
  if (pl.playing && !pl.paused && pl.has_next && !pl.nxt.fmt && !pl.next_warmed &&
      pl.cur.duration_ms > 0) {
    int64_t pos = pl.base_ms + pl.consumed * 1000 / RATE;
    if (pos + 8000 >= pl.cur.duration_ms) {
      warm_file(pl.next_path);
      pl.next_warmed = true;
    }
  }

  // Gapless: start the next station three seconds before this one ends, keep
  // its frames in their own ring, and swap when the current one is drained.
  if (pl.playing && !pl.paused && pl.has_next && !pl.nxt.fmt) {
    int64_t pos = pl.base_ms + pl.consumed * 1000 / RATE;
    if (pl.eof || (pl.cur.duration_ms > 0 && pos + 5000 >= pl.cur.duration_ms)) {
      warm_file(pl.next_path);
      if (!file_open(&pl.nxt, pl.next_path)) pl.has_next = false;
    }
  }

  if (pl.playing && !pl.paused && pl.nxt.fmt) fill_next(&pl);

  if (pl.manual_pending && pl.manual_fade && next_ring_len(&pl) >= 8) {
    int fade_blocks = (int)((long)pl.fade_ms * 1000 / 10667);
    pl.manual_pending = false;
    pl.fading = true;
    pl.fade_total = fade_blocks > 0 ? fade_blocks : 1;
    pl.fade_pos = 0;
  }

  if (!pl.fading && !pl.manual_fade && pl.playing && !pl.paused && pl.fade_ms > 0 && pl.has_next &&
      pl.nxt.fmt) {
    int fade_blocks = (int)((long)pl.fade_ms * 1000 / 10667);
    int left = ring_len(&pl);
    if (pl.eof && left > 0 && left <= fade_blocks && next_ring_len(&pl) >= left) {
      pl.fading = true;
      pl.fade_total = left;
      pl.fade_pos = 0;
    }
  }

  if (!pl.fading && !pl.manual_fade && pl.eof && ring_len(&pl) == 0 && next_ring_len(&pl) > 0)
    promote(&pl);

  if (!pl.primed && ring_len(&pl) >= RING_PRIME) pl.primed = true;

  uint64_t expirations = 0;
  if (h->tfd >= 0 && read(h->tfd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
    if (pl.primed && pl.playing && !pl.paused) {
      for (uint64_t i = 0; i < expirations && i < 4; i++) tick(&pl);
    }
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t ms = now.tv_sec * 1000LL + now.tv_nsec / 1000000;
  if (ms - h->last_status >= 100) {
    h->last_status = ms;
    struct player_status st;
    st.pos_ms = pl.base_ms + pl.consumed * 1000 / RATE;
    st.dur_ms = pl.cur.duration_ms;
    st.sent = pl.sent;
    st.underruns = pl.underruns;
    st.pushed = pl.pushed;
    st.paused = pl.paused ? 1 : 0;
    st.eof = (pl.eof && pl.tail == pl.head && pl.nxt.fmt == NULL && !pl.has_next) ? 1 : 0;
    if (pl.status_fn) pl.status_fn(&st, pl.sink_ud);
  }
}

void player_destroy(void *handle) {
  player_handle *h = (player_handle *)handle;
  if (!h) return;

  struct player &pl = h->pl;
  close_file(&pl);
  close_next(&pl);
  if (pl.hidraw >= 0) close(pl.hidraw);
  if (pl.enc) opus_encoder_destroy(pl.enc);
  if (pl.pkt) av_packet_free(&pl.pkt);
  if (pl.frame) av_frame_free(&pl.frame);
  if (h->tfd >= 0) close(h->tfd);
  free(h);
}

static void mode_status(const struct player_status *st, void *) {
  printf("pos %ld\ndur %ld\nsent %ld\nunderruns %ld\npushed %ld\npaused %d\neof %d\n", st->pos_ms,
         st->dur_ms, st->sent, st->underruns, st->pushed, st->paused, st->eof);
}

static void mode_track(const char *path, void *) { printf("track %s\n", path); }

int native_main(int argc, char **argv) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  void *h =
      player_create(argc > 1 ? argv[1] : nullptr, argc > 2 && strcmp(argv[2], "speaker") == 0);
  if (!h) return 1;
  player_set_sink(h, mode_status, mode_track, nullptr);

  int tfd = player_timer_fd(h);
  char line[1200];
  int line_len = 0;

  while (running) {
    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = tfd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    int ready = poll(fds, 2, 20);
    if (ready < 0 && errno != EINTR) break;

    if (fds[0].revents & POLLIN) {
      char buf[512];
      ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
      if (n <= 0) break;
      for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
          line[line_len] = 0;
          player_command(h, line);
          line_len = 0;
        } else if (line_len < (int)sizeof(line) - 1) {
          line[line_len++] = buf[i];
        }
      }
    }
    if (fds[1].revents & POLLIN) player_step(h);
  }

  player_destroy(h);
  return 0;
}
#undef printf
#undef RATE
#undef CHANNELS
#undef IN_FRAMES
#undef OPUS_FRAMES
#undef OPUS_BYTES
#undef REPORT_SIZE
#undef RING_BLOCKS
#undef RING_HIGH
#undef RING_PRIME
#undef DECODE_BUDGET_MS
// padwave input — the controller's state, decoded by the C++ schema.
//
// The decoder is an engine the app owns: input_create() takes the hidraw fd,
// input_set_sink() installs four callbacks (frame, action, event, mic) plus
// userdata, and input_feed() hands it bytes. The decoder keeps its edge state
// in the handle. The same code backs the `padwave input` CLI, which installs no
// callbacks and prints the actions/events/mic frames as lines for debugging.
//
// 0x31 (Bluetooth) and 0x01 (USB) are parsed with InputReport/InputBody from
// padwave_wire.hh. Fields are the names the panel speaks: buttons are the
// InputBody::Buttons enumerators, dpad is the InputBody::Dpad name, and the
// rest is straight from the body. battery/charging keep the same 10% steps and
// nibble 0/1/2 semantics.
//
//   padwave input [hidraw-node]
//   padwave input --hex 31c1...        one captured report, decode and exit
//   padwave input --file /path         a file of raw reports (testing)

// The decoder's state belongs to the handle, not the file: one handle per
// controller, carrying the callbacks and what the edge detectors remember.
namespace {

struct actions_state {
  uint32_t prev_buttons = 0;
  int64_t last_l2 = 0;
  int64_t last_r2 = 0;
  int64_t ps_at = 0;
  bool ps_armed = false;
};

struct sensors_state {
  bool has_prev = false;
  uint32_t prev_buttons = 0;
  uint8_t prev_dpad = 0;
  int prev_lx = 0, prev_ly = 0, prev_rx = 0, prev_ry = 0, prev_l2 = 0, prev_r2 = 0;
  bool prev_jack = false;
  int64_t last_activity = 0;
  int analog_resets = 0;
  int64_t last_shake = 0;
  int stable_jack = -1;
  int pending_jack = -1;
  int64_t jack_since = 0;
  bool l2_down = false, r2_down = false;
};

struct dpad_state {
  uint8_t prev = 0xFF;
  int hold_dir = 0;
  int hold_step = 0;
  int64_t hold_at = 0;
};

} // namespace

struct input_handle {
  int fd;
  input_frame_fn frame_fn;
  input_action_fn action_fn;
  input_event_fn event_fn;
  input_mic_fn mic_fn;
  void *ud;

  actions_state actions;
  sensors_state sensors;
  dpad_state dpad;
};

namespace {

volatile sig_atomic_t input_running = 1;

void input_on_signal(int) { input_running = 0; }

/* Poll then read, like hidapi's hid_read_timeout: some kernels do not report a
 * controller disconnect through a non-blocking read(), but poll() does with
 * POLLERR/POLLHUP/POLLNVAL. STDIN is polled too: when the parent closes the
 * port the CLI gets EOF there and exits instead of lingering with a stale
 * hidraw fd. Returns >0 bytes, 0 for a timeout/EAGAIN, -1 to stop. */
const char *dpad_name(uint8_t value) {
  switch (value) {
  case padwave::InputBody::Up: return "up";
  case padwave::InputBody::UpRight: return "up_right";
  case padwave::InputBody::Right: return "right";
  case padwave::InputBody::DownRight: return "down_right";
  case padwave::InputBody::Down: return "down";
  case padwave::InputBody::DownLeft: return "down_left";
  case padwave::InputBody::Left: return "left";
  case padwave::InputBody::UpLeft: return "up_left";
  default: return "none";
  }
}

void buttons_to_string(uint32_t buttons, char *out, size_t cap) {
  struct {
    uint32_t bit;
    const char *name;
  } names[] = {
      {padwave::InputBody::ButtonSquare, "square"},
      {padwave::InputBody::ButtonCross, "cross"},
      {padwave::InputBody::ButtonCircle, "circle"},
      {padwave::InputBody::ButtonTriangle, "triangle"},
      {padwave::InputBody::L1, "l1"},
      {padwave::InputBody::R1, "r1"},
      {padwave::InputBody::L2, "l2"},
      {padwave::InputBody::R2, "r2"},
      {padwave::InputBody::Create, "create"},
      {padwave::InputBody::Options, "options"},
      {padwave::InputBody::L3, "l3"},
      {padwave::InputBody::R3, "r3"},
      {padwave::InputBody::Ps, "ps"},
      {padwave::InputBody::Touchpad, "touchpad"},
      {padwave::InputBody::Mute, "mute"},
      {padwave::InputBody::Fn1, "fn1"},
      {padwave::InputBody::Fn2, "fn2"},
      {padwave::InputBody::LeftPaddle, "left_paddle"},
      {padwave::InputBody::RightPaddle, "right_paddle"},
  };
  size_t o = 0;
  out[0] = 0;
  for (auto &n : names) {
    if (!(buttons & n.bit)) continue;
    int w = snprintf(out + o, cap - o, "%s%s", o ? "," : "", n.name);
    if (w <= 0 || (size_t)w >= cap - o) break;
    o += (size_t)w;
  }
}

/* -- d-pad steering -------------------------------------------------------
 * The d-pad's timing lives here now: a tap seeks 6 s, a hold ramps through
 * 10/20/40/60 s at 420 ms, then repeats the last step at 130 ms. The decoder
 * reports (name, value) actions and the app only executes them. */
/* -- buttons and triggers -------------------------------------------------
 * The static bindings table moves here with the d-pad. The radio only executes
 * the actions; the UI's bindings map is documentation, not logic. */
int64_t input_now_ms();
void emit_action(input_handle *h, const char *name, int value);
void emit_event(input_handle *h, const char *name);

struct binding {
  uint32_t bit;
  const char *action;
};

const binding kBindings[] = {
    {padwave::InputBody::ButtonTriangle, "toggle_pause"},
    {padwave::InputBody::ButtonCircle, "play"},
    {padwave::InputBody::ButtonSquare, "shuffle_game"},
    {padwave::InputBody::L1, "previous"},
    {padwave::InputBody::R1, "next_random"},
    {padwave::InputBody::Create, "previous"},
    {padwave::InputBody::Options, "next_random"},
    {padwave::InputBody::Touchpad, "toggle_mute"},
    {padwave::InputBody::Mute, "toggle_mute"},
    {padwave::InputBody::L3, "next_random"},
    {padwave::InputBody::R3, "previous"},
    {padwave::InputBody::LeftPaddle, "previous"},
    {padwave::InputBody::RightPaddle, "next_random"},
    {padwave::InputBody::Fn1, "toggle_pause"},
    {padwave::InputBody::Fn2, "toggle_mute"},
};

void track_buttons(input_handle *h, uint32_t buttons) {
  uint32_t pressed = buttons & ~h->actions.prev_buttons;
  h->actions.prev_buttons = buttons;

  for (auto &b : kBindings)
    if (pressed & b.bit) emit_action(h, b.action, 0);

  if (pressed & padwave::InputBody::Ps) {
    h->actions.ps_armed = true;
    h->actions.ps_at = input_now_ms();
  }

  if (!(buttons & padwave::InputBody::Ps)) {
    h->actions.ps_armed = false;
  } else if (h->actions.ps_armed && input_now_ms() - h->actions.ps_at >= 2000) {
    emit_action(h, "suspend", 0);
    h->actions.ps_armed = false;
  }
}

void track_triggers(input_handle *h, int l2, int r2) {
  int64_t now = input_now_ms();
  if (l2 >= 30 && now - h->actions.last_l2 >= 110) {
    int step = l2 >= 220 ? 4 : (l2 >= 150 ? 2 : 1);
    emit_action(h, "volume", -step);
    h->actions.last_l2 = now;
  }
  if (r2 >= 30 && now - h->actions.last_r2 >= 110) {
    int step = r2 >= 220 ? 4 : (r2 >= 150 ? 2 : 1);
    emit_action(h, "volume", step);
    h->actions.last_r2 = now;
  }
}

/* -- sensors and the remaining controller events -------------------------
 * Activity/wake (with the analog cap), shake, the 250 ms jack debounce and the
 * trigger actuator's pressed/released edges. The radio still executes the
 * actions; these are announcements, not decisions. */
void track_sensors(input_handle *h, const padwave::InputBody &b) {
  int64_t now = input_now_ms();
  bool jack = (b.status1.get() & padwave::InputBody::Headset) != 0;
  int l2 = b.l2.get(), r2 = b.r2.get();
  int lx = b.lx.get(), ly = b.ly.get(), rx = b.rx.get(), ry = b.ry.get();
  uint32_t buttons = b.buttons;
  uint8_t dpad = (uint8_t)(buttons & 0x0F);
  bool first = !h->sensors.has_prev;

  bool real = false, analog = false;
  if (!first) {
    real = buttons != h->sensors.prev_buttons || dpad != h->sensors.prev_dpad ||
           jack != h->sensors.prev_jack || abs(l2 - h->sensors.prev_l2) > 10 ||
           abs(r2 - h->sensors.prev_r2) > 10;
    analog = abs(lx - h->sensors.prev_lx) > 24 || abs(ly - h->sensors.prev_ly) > 24 ||
             abs(rx - h->sensors.prev_rx) > 24 || abs(ry - h->sensors.prev_ry) > 24;
  }

  double g =
      sqrt((double)b.accel_x.get() * b.accel_x.get() + (double)b.accel_y.get() * b.accel_y.get() +
           (double)b.accel_z.get() * b.accel_z.get()) /
      8192.0;
  if (g >= 2.25 && now - h->sensors.last_shake > 1500) {
    emit_event(h, "shake");
    h->sensors.last_shake = now;
    real = true;
  }

  if (real || analog) {
    bool capped = analog && !real && h->sensors.analog_resets >= 6;
    if (!capped && now - h->sensors.last_activity > 900) {
      emit_event(h, "activity");
      h->sensors.last_activity = now;
      if (analog)
        h->sensors.analog_resets++;
      else
        h->sensors.analog_resets = 0;
    } else if (real) {
      h->sensors.analog_resets = 0;
    }
  }

  if (first) {
    h->sensors.stable_jack = jack ? 1 : 0;
  } else if (jack == (h->sensors.stable_jack != 0)) {
    h->sensors.pending_jack = -1;
    h->sensors.jack_since = 0;
  } else if (h->sensors.pending_jack != (jack ? 1 : 0) || h->sensors.jack_since == 0) {
    h->sensors.pending_jack = jack ? 1 : 0;
    h->sensors.jack_since = now;
  } else if (now - h->sensors.jack_since >= 250) {
    emit_action(h, jack ? "jack_plugged" : "jack_unplugged", 0);
    h->sensors.stable_jack = jack ? 1 : 0;
    h->sensors.pending_jack = -1;
    h->sensors.jack_since = 0;
  }

  if (!h->sensors.l2_down && l2 >= 30) {
    emit_event(h, "trigger_l_pressed");
    h->sensors.l2_down = true;
  } else if (h->sensors.l2_down && l2 < 15) {
    emit_event(h, "trigger_l_released");
    h->sensors.l2_down = false;
  }
  if (!h->sensors.r2_down && r2 >= 30) {
    emit_event(h, "trigger_r_pressed");
    h->sensors.r2_down = true;
  } else if (h->sensors.r2_down && r2 < 15) {
    emit_event(h, "trigger_r_released");
    h->sensors.r2_down = false;
  }

  h->sensors.has_prev = true;
  h->sensors.prev_buttons = buttons;
  h->sensors.prev_dpad = dpad;
  h->sensors.prev_lx = lx;
  h->sensors.prev_ly = ly;
  h->sensors.prev_rx = rx;
  h->sensors.prev_ry = ry;
  h->sensors.prev_l2 = l2;
  h->sensors.prev_r2 = r2;
  h->sensors.prev_jack = jack;
}

int64_t input_now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// The handle without a callback is the CLI: keep the lines for debugging.
void emit_action(input_handle *h, const char *name, int value) {
  if (h->action_fn) {
    h->action_fn(name, value, h->ud);
    return;
  }
  if (value)
    printf("action %s %d\n", name, value);
  else
    printf("action %s\n", name);
  fflush(stdout);
}

void emit_event(input_handle *h, const char *name) {
  if (h->event_fn) {
    h->event_fn(name, h->ud);
    return;
  }
  printf("event %s\n", name);
  fflush(stdout);
}

void track_dpad(input_handle *h, uint32_t buttons) {
  static const int steps[] = {10, 20, 40, 60};
  static const int intervals[] = {320, 260, 190, 130};

  uint8_t dpad = (uint8_t)(buttons & 0x0F);
  int64_t now = input_now_ms();

  if (dpad != h->dpad.prev) {
    h->dpad.prev = dpad;
    h->dpad.hold_dir = 0;

    switch (dpad) {
    case padwave::InputBody::Left:
      emit_action(h, "seek", -6);
      h->dpad.hold_dir = -1;
      h->dpad.hold_step = 0;
      h->dpad.hold_at = now + 420;
      break;
    case padwave::InputBody::Right:
      emit_action(h, "seek", 6);
      h->dpad.hold_dir = 1;
      h->dpad.hold_step = 0;
      h->dpad.hold_at = now + 420;
      break;
    case padwave::InputBody::Up: emit_action(h, "next_random", 0); break;
    case padwave::InputBody::Down: emit_action(h, "previous", 0); break;
    default: break;
    }
    return;
  }

  if (h->dpad.hold_dir == 0 || now < h->dpad.hold_at) return;

  int index = h->dpad.hold_step < 4 ? h->dpad.hold_step : 3;
  emit_action(h, "seek", h->dpad.hold_dir * steps[index]);
  h->dpad.hold_step = index + 1;
  h->dpad.hold_at = now + intervals[index];
}

void emit_frame(input_handle *h, const padwave::InputBody &b, const uint8_t *raw, size_t len) {
  struct input_frame f;
  memset(&f, 0, sizeof(f));
  uint32_t buttons = b.buttons;
  f.seq = b.seq.get();
  f.lx = b.lx.get();
  f.ly = b.ly.get();
  f.rx = b.rx.get();
  f.ry = b.ry.get();
  f.l2 = b.l2.get();
  f.r2 = b.r2.get();
  snprintf(f.dpad, sizeof(f.dpad), "%s", dpad_name(buttons & 0x0F));
  buttons_to_string(buttons, f.buttons, sizeof(f.buttons));
  f.battery = (b.status0 & 0x0F) * 10 + 5;
  if (f.battery > 100) f.battery = 100;
  f.charging = b.status0.get() >> 4;
  f.jack = (b.status1.get() & padwave::InputBody::Headset) ? 1 : 0;
  f.mic = (b.status1.get() & padwave::InputBody::Microphone) ? 1 : 0;
  f.temp = b.temperature.get();
  f.gx = b.gyro_x.get();
  f.gy = b.gyro_y.get();
  f.gz = b.gyro_z.get();
  f.ax = b.accel_x.get();
  f.ay = b.accel_y.get();
  f.az = b.accel_z.get();
  f.tfl = b.trigger_feedback_l.get();
  f.tfr = b.trigger_feedback_r.get();
  for (int t = 0; t < 2; t++) {
    const PWUINT8 *touch = t == 0 ? b.touch1 : b.touch2;
    unsigned contact = touch[0].get();
    unsigned middle = touch[2].get();
    f.touches[t].active = (contact & 0x80) ? 0 : 1;
    f.touches[t].id = contact & 0x7F;
    f.touches[t].x = touch[1].get() | ((middle & 0x0F) << 8);
    f.touches[t].y = (middle >> 4) | (touch[3].get() << 4);
  }
  size_t r = len < (sizeof(f.raw) - 1) / 2 ? len : (sizeof(f.raw) - 1) / 2;
  for (size_t i = 0; i < r; i++) snprintf(f.raw + i * 2, sizeof(f.raw) - i * 2, "%02x", raw[i]);

  if (h->frame_fn) h->frame_fn(&f, h->ud);

  track_dpad(h, b.buttons);
  track_buttons(h, b.buttons);
  track_triggers(h, b.l2, b.r2);
  track_sensors(h, b);
}

// One report: 0x31 Bluetooth (78), 0x01 USB (64), or a 0x31 microphone frame.
void handle_report(input_handle *h, const uint8_t *buf, size_t len) {
  if (len > 7 && padwave::MicFrame::is_frame(buf, len)) {
    size_t payload_len = 0;
    const uint8_t *payload = padwave::MicFrame::payload(buf, len, &payload_len);
    if (h->mic_fn) {
      h->mic_fn(payload, payload_len, h->ud);
    } else {
      printf("mic ");
      for (size_t i = 0; i < payload_len; i++) printf("%02x", payload[i]);
      printf("\n");
      fflush(stdout);
    }
    return;
  }

  if (buf[0] == padwave::PacketStateBluetooth && len >= padwave::InputReport::Size) {
    const padwave::InputReport *r = (const padwave::InputReport *)buf;
    if (!r->valid()) return;
    emit_frame(h, r->body, buf, padwave::InputReport::Size);
    return;
  }

  if (buf[0] == padwave::PacketInputUsb && len >= padwave::InputReport::UsbSize)
    emit_frame(h, *(const padwave::InputBody *)(buf + 1), buf, padwave::InputReport::UsbSize);
}

bool hex_decode(const char *hex, uint8_t *out, size_t *out_len) {
  size_t n = strlen(hex);
  if (n % 2) return false;
  size_t bytes = n / 2;
  if (bytes > 512) return false;
  for (size_t i = 0; i < bytes; i++) {
    unsigned v = 0;
    if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  *out_len = bytes;
  return true;
}

} // namespace

void input_set_sink(void *handle, input_frame_fn frame, input_action_fn action,
                    input_event_fn event, input_mic_fn mic, void *ud) {
  input_handle *h = (input_handle *)handle;
  if (!h) return;
  h->frame_fn = frame;
  h->action_fn = action;
  h->event_fn = event;
  h->mic_fn = mic;
  h->ud = ud;
}

void *input_create(const char *node) {
  int fd = -1;
  if (node) {
    fd = open(node, O_RDONLY | O_CLOEXEC);
  } else {
    char found[64];
    if (find_dualsense(found, sizeof(found)) >= 0) fd = open(found, O_RDONLY | O_CLOEXEC);
  }
  if (fd < 0) {
    fprintf(stderr, "padwave input: no DualSense hidraw node\n");
    return nullptr;
  }

  input_handle *h = (input_handle *)calloc(1, sizeof(*h));
  if (!h) {
    close(fd);
    return nullptr;
  }
  *h = input_handle{};
  h->fd = fd;
  return h;
}

int input_fd(void *handle) { return handle ? ((input_handle *)handle)->fd : -1; }

int input_feed(void *handle, const uint8_t *buf, size_t len) {
  input_handle *h = (input_handle *)handle;
  if (!h) return -1;
  handle_report(h, buf, len);
  return 0;
}

void input_destroy(void *handle) {
  input_handle *h = (input_handle *)handle;
  if (!h) return;
  if (h->fd >= 0) close(h->fd);
  free(h);
}

int input_main(int argc, char **argv) {
  uint8_t buf[512];
  input_handle scratch{};

  if (argc > 1 && strcmp(argv[1], "--hex") == 0 && argc > 2) {
    size_t len = 0;
    if (!hex_decode(argv[2], buf, &len)) return 2;
    if (len > 1 && (buf[0] == 0xA1 || buf[0] == 0xA2)) {
      memmove(buf, buf + 1, len - 1);
      len--;
    }
    handle_report(&scratch, buf, len);
    return 0;
  }

  if (argc > 1 && strcmp(argv[1], "--file") == 0 && argc > 2) {
    int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 2;
    for (;;) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) break;
      handle_report(&scratch, buf, (size_t)n);
    }
    close(fd);
    return 0;
  }

  signal(SIGINT, input_on_signal);
  signal(SIGTERM, input_on_signal);

  void *h = input_create(argc > 1 ? argv[1] : nullptr);
  if (!h) return 1;
  printf("ready\n");

  int fd = input_fd(h);
  while (input_running) {
    struct pollfd p[2];
    p[0].fd = fd;
    p[0].events = POLLIN;
    p[0].revents = 0;
    p[1].fd = STDIN_FILENO;
    p[1].events = POLLIN;
    p[1].revents = 0;

    int ret = poll(p, 2, 1000);
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      char discard;
      if (read(STDIN_FILENO, &discard, 1) <= 0) break;
    }

    if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      printf("down\n");
      break;
    }
    if (p[0].revents & POLLIN) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
          printf("down\n");
          break;
        }
        continue;
      }
      input_feed(h, buf, (size_t)n);
    }
  }

  input_destroy(h);
  return 0;
}
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

extern "C" {}

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

int server_main(int argc, char **argv) {
  static Server server{}; // connection table in BSS; the App stays on the stack
  int port = argc > 1 ? atoi(argv[1]) : 8787;
  return server.run(port);
}

// padwave — one binary, the native modes.
//
//   padwave native            the player: decode, Opus and 0x36/0x31
//   padwave input             the input decoder: 0x31 -> frame/action/event/mic
//   padwave serve [port]      the web server and the app (epoll, embedded UI)
//   padwave opusdec [-e|-s]   the codec helper
//
// All of them include padwave_wire.hh, so the protocol is described
// once and no mode redefines a packet.

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "opusdec") == 0) return opusdec_main(argc - 1, argv + 1);
  if (argc > 1 && strcmp(argv[1], "input") == 0) return input_main(argc - 1, argv + 1);
  if (argc > 1 && strcmp(argv[1], "serve") == 0) return server_main(argc - 1, argv + 1);
  return native_main(argc - 1, argv + 1);
}