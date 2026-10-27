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



// Entry points and in-process engines. The service calls the player/input
// engines directly from its epoll loop; the *_main wrappers keep the debug
// modes working.

int native_main(int argc, char **argv);
int input_main(int argc, char **argv);
int opusdec_main(int argc, char **argv);
int server_main(int argc, char **argv);

void *player_create(const char *node, bool speaker);
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

void player_set_sink(void *handle, player_status_fn status, player_track_fn track, void *ud);
int player_timer_fd(void *handle);
void player_step(void *handle);
void player_command(void *handle, const char *line);
void player_destroy(void *handle);

void *input_create(const char *node);
struct input_touch {
  unsigned active;
  unsigned id;
  unsigned x, y;
};
struct input_frame {
  unsigned seq;
  unsigned lx, ly, rx, ry, l2, r2;
  char dpad[16];
  char buttons[256];
  int battery;
  unsigned charging;
  int jack;
  int mic;
  unsigned temp;
  int gx, gy, gz;
  int ax, ay, az;
  unsigned tfl, tfr;
  struct input_touch touches[2];
  char raw[512];
};
typedef void (*input_frame_fn)(const struct input_frame *, void *);
typedef void (*input_action_fn)(const char *name, int value, void *);
typedef void (*input_event_fn)(const char *name, void *);
typedef void (*input_mic_fn)(const uint8_t *data, size_t len, void *);

void input_set_sink(void *handle, input_frame_fn frame, input_action_fn action,
                    input_event_fn event, input_mic_fn mic, void *ud);
int input_fd(void *handle);
int input_feed(void *handle, const uint8_t *buf, size_t len);
void input_destroy(void *handle);

// The standalone app. The server owns one instance (a stack object in
// server_main) and passes it to the callbacks as userdata.

struct app;

void app_tick(struct app *a);
int app_fds(struct app *a, int *fds, int max);
void app_handle_fd(struct app *a, int fd);
int app_status_text(struct app *a, char *buf, size_t cap);
int app_front_text(struct app *a, char *buf, size_t cap, unsigned long requests, long uptime);
int app_command(struct app *a, const char *action, const char *body, size_t body_len,
                char *out, size_t out_cap);
int app_handle_command(struct app *a, const char *body, size_t len, char *out, size_t cap);
bool app_owns_fd(struct app *a, int fd);
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
#define RING_BLOCKS 192 /* about two seconds: enough for a 1.5 s crossfade */
#define RING_HIGH 160   /* stop decoding above this many buffered blocks */
#define RING_PRIME 24  /* start sending once this many are buffered */
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
  int64_t base_ms;   /* source time at the start of the ring */
  int64_t consumed;  /* input frames taken out of the ring */
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

  if (pl->hidraw >= 0) {
    ssize_t n = write(pl->hidraw, &report, sizeof(report));
    (void)n;
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
                      float acc[IN_FRAMES][CHANNELS], int *acc_frames, int high,
                      int64_t *pushed) {
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
  fill_into(pl, &pl->nxt, pl->nring, &pl->nhead, &pl->ntail, pl->nacc, &pl->nacc_frames,
            RING_HIGH, &pl->npushed);
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
  printf("pos %ld\ndur %ld\nsent %ld\nunderruns %ld\npushed %ld\npaused %d\neof %d\n",
         st->pos_ms, st->dur_ms, st->sent, st->underruns, st->pushed, st->paused, st->eof);
}

static void mode_track(const char *path, void *) { printf("track %s\n", path); }

int native_main(int argc, char **argv) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  void *h = player_create(argc > 1 ? argv[1] : nullptr,
                          argc > 2 && strcmp(argv[2], "speaker") == 0);
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
      {padwave::InputBody::ButtonSquare, "square"}, {padwave::InputBody::ButtonCross, "cross"},
      {padwave::InputBody::ButtonCircle, "circle"}, {padwave::InputBody::ButtonTriangle, "triangle"},
      {padwave::InputBody::L1, "l1"},               {padwave::InputBody::R1, "r1"},
      {padwave::InputBody::L2, "l2"},               {padwave::InputBody::R2, "r2"},
      {padwave::InputBody::Create, "create"},       {padwave::InputBody::Options, "options"},
      {padwave::InputBody::L3, "l3"},               {padwave::InputBody::R3, "r3"},
      {padwave::InputBody::Ps, "ps"},               {padwave::InputBody::Touchpad, "touchpad"},
      {padwave::InputBody::Mute, "mute"},           {padwave::InputBody::Fn1, "fn1"},
      {padwave::InputBody::Fn2, "fn2"},             {padwave::InputBody::LeftPaddle, "left_paddle"},
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

  double g = sqrt((double)b.accel_x.get() * b.accel_x.get() +
                  (double)b.accel_y.get() * b.accel_y.get() +
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
      if (analog) h->sensors.analog_resets++;
      else h->sensors.analog_resets = 0;
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
  if (value) printf("action %s %d\n", name, value);
  else printf("action %s\n", name);
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
      case padwave::InputBody::Up:
        emit_action(h, "next_random", 0);
        break;
      case padwave::InputBody::Down:
        emit_action(h, "previous", 0);
        break;
      default:
        break;
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

extern "C" {
}


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
}// The standalone padwave app: it owns the player and input children and the
// station state the panel reads. Deliberately plain C++ (no STL, no
// exceptions), like the rest of the native side.




constexpr int kMaxStations = 4096;
constexpr int kMaxPath = 1024;
constexpr int kMaxTitle = 160;
constexpr int kMaxGame = 96;
constexpr int kMaxHistory = 48;

struct station {
  char path[kMaxPath];
  char title[kMaxTitle];
  char game[kMaxGame];
  char ext[8];
  char id[17];
};

struct app {
  station stations[kMaxStations];
  int count = 0;
  int current = -1;
  int history[kMaxHistory];
  int history_len = 0;

  void *player = nullptr;
  void *input = nullptr;
  int player_timer_fd = -1;
  int input_fd = -1;

  long pos_ms = 0;
  long dur_ms = 0;
  long sent = 0;
  long underruns = 0;
  bool eof = false;
  bool playing = false;
  bool paused = false;
  int volume = 70;
  bool muted = false;
  long start_ms = 0;

  bool controller = false;
  bool jack = false;
  int battery = -1;
  char charging[16] = "unknown";
  int lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
  char dpad[16] = "none";
  char buttons[256] = "";
  int temp = 0;
  int ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  unsigned seq = 0;
  int rate = 0;
  unsigned tfl = 0, tfr = 0;
  struct input_touch touch[2] = {};
  long rate_at = 0;
  unsigned rate_frames = 0;
  char input_node[64] = "";
  char last_action[64] = "";
  char raw[400] = "";

  int reconnect_at = 0;
  long idle_since = 0;

  // Config that the panel edits; persisted as ~/.config/padwave/config.
  int idle_timeout_ms = 900000;
  int idle_warn_grace_ms = 10000;
  int trigger_effect_amplitude = 6;
  int trigger_effect_position = 2;
  int trigger_effect_frequency = 25;
  bool trigger_effects = true;

  int rumble_left = 0, rumble_right = 0;
  long rumble_until = 0;
  int trigger_side = 0;  // 1 left, 2 right; 0 idle
  long trigger_until = 0;

  void add_station(const char *path, const char *game);
  void scan_dir(const char *dir, const char *game);
  int find_station(const char *path);
  int find_id(const char *id);
  void push_history(int index);
  void player_send(const char *line);
  void player_start();
  void input_start();
  void input_stop();
  static void player_status_cb(const struct player_status *st, void *ud);
  static void player_track_cb(const char *path, void *ud);
  void play(int index, long start_ms);
  void next();
  void previous();
  void shuffle_game();
  void toggle_pause();
  void set_volume(int v);
  void seek(long ms);
  static void input_frame_cb(const struct input_frame *f, void *ud);
  static void input_action_cb(const char *name, int value, void *ud);
  static void input_event_cb(const char *name, void *ud);
  static void input_mic_cb(const uint8_t *data, size_t len, void *ud);
  void handle_action(const char *name, int value);
  int status_text(char *buf, size_t cap);
  int front_text(char *buf, size_t cap, unsigned long requests, long uptime);
  int config_text(char *buf, size_t cap);
  void config_set(const char *body);
  void config_load();
  void config_save();
  void output_fields(const char *fields);
  int output(const char *body, char *out, size_t out_cap);
  void service_output();
  int library_text(char *buf, size_t cap);
  int command(const char *action, const char *body, size_t body_len, char *out, size_t out_cap);
  int handle_command(const char *body, size_t len, char *out, size_t cap);
  bool owns_fd(int fd);
  bool init();
  void shutdown();
  void tick();
  int fds(int *fds, int max);
  void handle_fd(int fd);
};


long now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

void make_id(const char *path, char *out) {
  uint64_t h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  snprintf(out, 17, "%016llx", (unsigned long long)h);
}

bool audio_ext(const char *name) {
  const char *dot = strrchr(name, '.');
  if (!dot) return false;
  static const char *exts[] = {".flac", ".wav", ".mp3", ".ogg", ".opus", ".m4a", ".aac", ".wma"};
  for (const char *e : exts)
    if (strcasecmp(dot, e) == 0) return true;
  return false;
}

void app::add_station(const char *path, const char *game) {
  if (this->count >= kMaxStations) return;
  station *s = &this->stations[this->count];
  snprintf(s->path, sizeof(s->path), "%s", path);

  const char *base = strrchr(path, '/');
  snprintf(s->title, sizeof(s->title), "%.*s", (int)sizeof(s->title) - 1, base ? base + 1 : path);
  char *dot = strrchr(s->title, '.');
  if (dot) *dot = 0;

  snprintf(s->game, sizeof(s->game), "%.*s", (int)sizeof(s->game) - 1, game);
  const char *d = strrchr(path, '.');
  snprintf(s->ext, sizeof(s->ext), "%s", d ? d + 1 : "");
  make_id(path, s->id);
  this->count++;
}

void app::scan_dir(const char *dir, const char *game) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char path[kMaxPath];
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    struct stat st;
    if (lstat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      scan_dir(path, e->d_name);
    } else if (S_ISREG(st.st_mode) && audio_ext(e->d_name)) {
      add_station(path, game);
    }
  }
  closedir(d);
}

int cmp_station(const void *a, const void *b) {
  const station *x = (const station *)a;
  const station *y = (const station *)b;
  int c = strcasecmp(x->game, y->game);
  return c ? c : strcasecmp(x->title, y->title);
}

int app::find_station(const char *path) {
  for (int i = 0; i < this->count; i++)
    if (strcmp(this->stations[i].path, path) == 0) return i;
  return -1;
}

int app::find_id(const char *id) {
  for (int i = 0; i < this->count; i++)
    if (strcmp(this->stations[i].id, id) == 0) return i;
  return -1;
}

void app::push_history(int index) {
  if (index < 0) return;
  if (this->history_len < kMaxHistory) {
    this->history[this->history_len++] = index;
  } else {
    memmove(this->history, this->history + 1, sizeof(int) * (kMaxHistory - 1));
    this->history[kMaxHistory - 1] = index;
  }
}

void app::player_send(const char *line) {
  if (this->player) ::player_command(this->player, line);
}

void app::player_start() {
  if (this->player) return;
  this->player = ::player_create(nullptr, false);
  if (!this->player) return;
  ::player_set_sink(this->player, player_status_cb, player_track_cb, this);
  this->player_timer_fd = ::player_timer_fd(this->player);
}

void app::input_start() {
  if (this->input) return;

  char node[64];
  if (find_dualsense(node, sizeof(node)) < 0) {
    this->reconnect_at = (int)(now_ms() + 1500);
    return;
  }

  this->input = ::input_create(node);
  if (!this->input) {
    this->reconnect_at = (int)(now_ms() + 1500);
    return;
  }
  snprintf(this->input_node, sizeof(this->input_node), "%s", node);
  ::input_set_sink(this->input, input_frame_cb, input_action_cb, input_event_cb, input_mic_cb,
                   this);
  this->input_fd = ::input_fd(this->input);
}

void app::input_stop() {
  if (this->input) {
    ::input_destroy(this->input);
    this->input = nullptr;
  }
  this->input_fd = -1;
  this->controller = false;
  this->input_node[0] = 0;
  this->rate = 0;
  this->rate_at = 0;
  this->rate_frames = 0;
}

void app::player_status_cb(const struct player_status *st, void *ud) {
  app *a = (app *)ud;
  a->pos_ms = st->pos_ms;
  a->dur_ms = st->dur_ms;
  a->sent = st->sent;
  a->underruns = st->underruns;
  a->paused = st->paused != 0;
  a->eof = st->eof != 0;
}

void app::player_track_cb(const char *path, void *ud) {
  app *a = (app *)ud;
  int idx = a->find_station(path);
  if (idx < 0) return;
  if (a->current >= 0 && a->current != idx) a->push_history(a->current);
  a->current = idx;
  a->paused = false;
  a->playing = true;
}

void app::play(int index, long start_ms) {
  if (index < 0 || index >= this->count) return;
  player_start();
  char line[kMaxPath + 64];
  snprintf(line, sizeof(line), "fade 1500\nplay %ld %s\n", start_ms, this->stations[index].path);
  player_send(line);

  if (this->current >= 0 && this->current != index && this->history_len == 0) push_history(this->current);
  this->current = index;
  this->playing = true;
  this->paused = false;
  this->eof = false;
  this->pos_ms = start_ms;
  this->dur_ms = 0;
  this->idle_since = now_ms();

  // Gapless: queue a different station for the junction.
  if (this->count > 1) {
    int next = rand() % this->count;
    if (next == index) next = (next + 1) % this->count;
    char q[kMaxPath + 16];
    snprintf(q, sizeof(q), "queue %s\n", this->stations[next].path);
    player_send(q);
  }
}

void app::next() {
  if (this->count == 0) return;
  int next = rand() % this->count;
  if (this->count > 1 && next == this->current) next = (next + 1) % this->count;
  play(next, 0);
}

void app::previous() {
  if (this->history_len > 0) {
    int prev = this->history[--this->history_len];
    play(prev, 0);
  } else if (this->current > 0) {
    play(this->current - 1, 0);
  }
}

void app::shuffle_game() {
  if (this->count == 0) return;
  const char *game = this->current >= 0 ? this->stations[this->current].game : "";
  for (int tries = 0; tries < 100; tries++) {
    int i = rand() % this->count;
    if (strcmp(this->stations[i].game, game) != 0) {
      play(i, 0);
      return;
    }
  }
  next();
}

void app::toggle_pause() {
  if (!this->playing) {
    if (this->current >= 0)
      play(this->current, this->pos_ms);
    return;
  }
  this->paused = !this->paused;
  player_send(this->paused ? "pause 1\n" : "pause 0\n");
}

void app::set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  this->volume = v;
  this->muted = v == 0;
  char line[32];
  snprintf(line, sizeof(line), "volume %d\n", v);
  player_send(line);
}

void app::seek(long ms) {
  if (ms < 0) ms = 0;
  if (this->dur_ms > 0 && ms > this->dur_ms) ms = this->dur_ms;
  char line[32];
  snprintf(line, sizeof(line), "seek %ld\n", ms);
  player_send(line);
  this->pos_ms = ms;
}

void app::input_frame_cb(const struct input_frame *f, void *ud) {
  app *a = (app *)ud;
  a->controller = true;
  a->lx = (int)f->lx;
  a->ly = (int)f->ly;
  a->rx = (int)f->rx;
  a->ry = (int)f->ry;
  a->l2 = (int)f->l2;
  a->r2 = (int)f->r2;
  a->battery = f->battery;
  a->jack = f->jack != 0;
  a->temp = (int)f->temp;
  snprintf(a->dpad, sizeof(a->dpad), "%s", f->dpad);
  snprintf(a->buttons, sizeof(a->buttons), "%s", f->buttons);
  snprintf(a->raw, sizeof(a->raw), "%.*s", (int)sizeof(a->raw) - 1, f->raw);
  snprintf(a->charging, sizeof(a->charging), "%s",
           f->charging == 1 ? "charging" : f->charging == 2 ? "full" : "discharging");
  a->gx = f->gx;
  a->gy = f->gy;
  a->gz = f->gz;
  a->ax = f->ax;
  a->ay = f->ay;
  a->az = f->az;
  a->seq = f->seq;
  a->tfl = f->tfl;
  a->tfr = f->tfr;
  for (int i = 0; i < 2; i++) a->touch[i] = f->touches[i];

  long now = now_ms();
  if (a->rate_at == 0) a->rate_at = now;
  a->rate_frames++;
  if (now - a->rate_at >= 1000) {
    a->rate = (int)((a->rate_frames * 1000) / (now - a->rate_at));
    a->rate_frames = 0;
    a->rate_at = now;
  }
}

void app::input_action_cb(const char *name, int value, void *ud) {
  ((app *)ud)->handle_action(name, value);
}

// Events are announcements for a future listener; the app has nothing to do
// with them yet, but keeping the callback is what lets the decoder forget the
// text format.
void app::input_event_cb(const char *name, void *ud) {
  (void)name;
  (void)ud;
}

void app::input_mic_cb(const uint8_t *data, size_t len, void *ud) {
  (void)data;
  (void)len;
  (void)ud;
}

void app::handle_action(const char *name, int value) {
  if (strcmp(name, "next") == 0 || strcmp(name, "next_random") == 0) next();
  else if (strcmp(name, "previous") == 0) previous();
  else if (strcmp(name, "shuffle_game") == 0) shuffle_game();
  else if (strcmp(name, "pause") == 0 || strcmp(name, "toggle_pause") == 0) toggle_pause();
  else if (strcmp(name, "play") == 0) {
    if (!this->playing || this->paused) toggle_pause();
  } else if (strcmp(name, "mute") == 0 || strcmp(name, "toggle_mute") == 0) {
    set_volume(this->muted ? this->volume : 0);
  } else if (strcmp(name, "volume") == 0) {
    set_volume(this->volume + value);
  } else if (strcmp(name, "seek") == 0) {
    seek(this->pos_ms + (long)value * 1000);
  } else if (strcmp(name, "suspend") == 0 || strcmp(name, "disconnect") == 0) {
    this->playing = false;
    this->paused = true;
    player_send("pause 1\n");
  }
  snprintf(this->last_action, sizeof(this->last_action), "%s", name);
}

// The panel's hot wire: one key=value per line, dotted keys for nesting.
// Values are plain strings, numbers, true/false or null; index.html parses
// them back into the objects render() already understands. Library, config
// and schema keep their JSON shapes because they are structural, not polled.
void kv_add(char *buf, size_t cap, size_t *len, const char *key, const char *fmt, ...) {
  size_t start = *len;
  if (start + 2 > cap) return;
  int n = snprintf(buf + start, cap - start, "%s=", key);
  if (n <= 0 || (size_t)n >= cap - start) return;
  *len = start + (size_t)n;

  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + *len, cap - *len, fmt, ap);
  va_end(ap);
  if (m < 0 || (size_t)m + 2 > cap - *len) {
    *len = start;
    buf[start] = 0;
    return;
  }
  *len += (size_t)m;

  buf[(*len)++] = '\n';
  buf[*len] = 0;
}

void kv_text(char *buf, size_t cap, size_t *len, const char *key, const char *value) {
  size_t start = *len;
  if (start + 2 > cap) return;
  int n = snprintf(buf + start, cap - start, "%s=", key);
  if (n <= 0 || (size_t)n >= cap - start) return;
  *len = start + (size_t)n;

  for (const char *p = value; p && *p; p++) {
    if (*len + 2 >= cap) break;
    buf[(*len)++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
  }
  buf[(*len)++] = '\n';
  buf[*len] = 0;
}

int app::status_text(char *buf, size_t cap) {
  size_t o = 0;
  if (cap) buf[0] = 0;
  const station *s = this->current >= 0 ? &this->stations[this->current] : nullptr;

  kv_add(buf, cap, &o, "radio.playing", "%s", this->playing ? "true" : "false");
  kv_add(buf, cap, &o, "radio.paused", "%s", this->paused ? "true" : "false");
  kv_add(buf, cap, &o, "radio.position", "%.1f", this->pos_ms / 1000.0);
  kv_add(buf, cap, &o, "radio.duration", "%.1f", this->dur_ms / 1000.0);
  kv_add(buf, cap, &o, "radio.volume", "%d", this->volume);
  kv_add(buf, cap, &o, "radio.muted", "%s", this->muted ? "true" : "false");
  kv_add(buf, cap, &o, "radio.fade_percent", "%d", 100);
  if (s) {
    kv_text(buf, cap, &o, "radio.current.id", s->id);
    kv_text(buf, cap, &o, "radio.current.title", s->title);
    kv_text(buf, cap, &o, "radio.current.game", s->game);
    kv_text(buf, cap, &o, "radio.current.path", s->path);
    kv_text(buf, cap, &o, "radio.current.ext", s->ext);
  }

  kv_text(buf, cap, &o, "session.phase", "active");
  kv_add(buf, cap, &o, "session.warning_in", "%d", 900);
  kv_add(buf, cap, &o, "session.shutdown_in", "%d", 0);

  kv_add(buf, cap, &o, "audio.present", "%s", "false");
  kv_text(buf, cap, &o, "audio.sink", "");
  kv_text(buf, cap, &o, "audio.state", "null");

  kv_text(buf, cap, &o, "hid.power_save", "null");
  kv_text(buf, cap, &o, "hid.player_leds", "null");

  kv_add(buf, cap, &o, "mic.level", "%d", 0);
  kv_add(buf, cap, &o, "mic.active", "%s", "false");
  kv_add(buf, cap, &o, "mic.frames", "%d", 0);
  kv_add(buf, cap, &o, "mic.rms", "%d", 0);
  kv_add(buf, cap, &o, "mic.peak", "%d", 0);
  kv_add(buf, cap, &o, "mic.running", "%s", "false");
  kv_add(buf, cap, &o, "mic.monitor", "%s", "false");

  kv_add(buf, cap, &o, "controller.connected", "%s", this->controller ? "true" : "false");
  kv_text(buf, cap, &o, "controller.bus", "bluetooth");
  kv_text(buf, cap, &o, "controller.name", "DualSense");
  kv_text(buf, cap, &o, "controller.mac", "");
  kv_text(buf, cap, &o, "controller.model", "dualsense");
  kv_text(buf, cap, &o, "controller.node", this->input_node);
  if (this->battery >= 0) kv_add(buf, cap, &o, "controller.battery", "%d", this->battery);
  else kv_text(buf, cap, &o, "controller.battery", "null");
  kv_text(buf, cap, &o, "controller.charging", this->charging);
  kv_add(buf, cap, &o, "controller.jack", "%s", this->jack ? "true" : "false");
  kv_text(buf, cap, &o, "controller.buttons", this->buttons);
  kv_text(buf, cap, &o, "controller.dpad", this->dpad);
  kv_add(buf, cap, &o, "controller.rate", "%d", this->rate);
  kv_add(buf, cap, &o, "controller.fields.seq", "%u", this->seq);
  kv_add(buf, cap, &o, "controller.fields.tfl", "%u", this->tfl);
  kv_add(buf, cap, &o, "controller.fields.tfr", "%u", this->tfr);
  for (int t = 0; t < 2; t++) {
    char key[48];
    snprintf(key, sizeof(key), "controller.touches.%d.active", t);
    kv_add(buf, cap, &o, key, "%s", this->touch[t].active ? "true" : "false");
    snprintf(key, sizeof(key), "controller.touches.%d.id", t);
    kv_add(buf, cap, &o, key, "%u", this->touch[t].id);
    snprintf(key, sizeof(key), "controller.touches.%d.x", t);
    kv_add(buf, cap, &o, key, "%u", this->touch[t].x);
    snprintf(key, sizeof(key), "controller.touches.%d.y", t);
    kv_add(buf, cap, &o, key, "%u", this->touch[t].y);
  }
  kv_add(buf, cap, &o, "controller.l2", "%d", this->l2);
  kv_add(buf, cap, &o, "controller.r2", "%d", this->r2);
  kv_add(buf, cap, &o, "controller.lx", "%d", this->lx);
  kv_add(buf, cap, &o, "controller.ly", "%d", this->ly);
  kv_add(buf, cap, &o, "controller.rx", "%d", this->rx);
  kv_add(buf, cap, &o, "controller.ry", "%d", this->ry);
  kv_add(buf, cap, &o, "controller.temperature", "%d", this->temp);
  kv_add(buf, cap, &o, "controller.accel", "%d,%d,%d", this->ax, this->ay, this->az);
  kv_add(buf, cap, &o, "controller.gyro", "%d,%d,%d", this->gx, this->gy, this->gz);
  kv_text(buf, cap, &o, "controller.raw", this->raw);
  kv_text(buf, cap, &o, "controller.last_action", this->last_action);

  kv_add(buf, cap, &o, "config.idle_timeout_ms", "%d", this->idle_timeout_ms);
  kv_add(buf, cap, &o, "config.idle_warn_grace_ms", "%d", this->idle_warn_grace_ms);
  kv_add(buf, cap, &o, "config.trigger_effect_amplitude", "%d", this->trigger_effect_amplitude);
  kv_add(buf, cap, &o, "config.trigger_effect_position", "%d", this->trigger_effect_position);
  kv_add(buf, cap, &o, "config.trigger_effect_frequency", "%d", this->trigger_effect_frequency);
  kv_add(buf, cap, &o, "config.trigger_effects", "%s", this->trigger_effects ? "true" : "false");
  return (int)o;
}

int app::front_text(char *buf, size_t cap, unsigned long requests, long uptime) {
  size_t o = 0;
  if (cap) buf[0] = 0;
  kv_add(buf, cap, &o, "ok", "%s", "true");
  kv_add(buf, cap, &o, "front", "%d", 1);
  kv_add(buf, cap, &o, "uptime_s", "%ld", uptime);
  kv_add(buf, cap, &o, "requests", "%lu", requests);
  kv_add(buf, cap, &o, "controller", "%s", this->controller ? "true" : "false");
  kv_add(buf, cap, &o, "player.valid", "%s", this->player ? "true" : "false");
  kv_add(buf, cap, &o, "player.pos", "%ld", this->pos_ms);
  kv_add(buf, cap, &o, "player.dur", "%ld", this->dur_ms);
  kv_add(buf, cap, &o, "player.sent", "%ld", this->sent);
  kv_add(buf, cap, &o, "player.underruns", "%ld", this->underruns);
  kv_add(buf, cap, &o, "player.paused", "%s", this->paused ? "true" : "false");
  kv_add(buf, cap, &o, "input.valid", "%s", this->input ? "true" : "false");
  kv_add(buf, cap, &o, "input.lx", "%d", this->lx);
  kv_add(buf, cap, &o, "input.ly", "%d", this->ly);
  kv_add(buf, cap, &o, "input.rx", "%d", this->rx);
  kv_add(buf, cap, &o, "input.ry", "%d", this->ry);
  kv_add(buf, cap, &o, "input.l2", "%d", this->l2);
  kv_add(buf, cap, &o, "input.r2", "%d", this->r2);
  kv_add(buf, cap, &o, "input.battery", "%d", this->battery);
  kv_add(buf, cap, &o, "input.jack", "%s", this->jack ? "true" : "false");
  return (int)o;
}

// One track per line, tab-separated: id, title, game, path, ext. Tabs and
// newlines inside a path are flattened to spaces so the line stays parseable.
static void tsv_field(char *buf, size_t cap, size_t *o, const char *value, char sep) {
  for (const char *p = value; p && *p && *o + 2 < cap; p++)
    buf[(*o)++] = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
  if (*o + 1 < cap) buf[(*o)++] = sep;
  buf[*o] = 0;
}

int app::library_text(char *buf, size_t cap) {
  size_t o = 0;
  if (cap) buf[0] = 0;
  for (int i = 0; i < this->count; i++) {
    const station *s = &this->stations[i];
    tsv_field(buf, cap, &o, s->id, '\t');
    tsv_field(buf, cap, &o, s->title, '\t');
    tsv_field(buf, cap, &o, s->game, '\t');
    tsv_field(buf, cap, &o, s->path, '\t');
    tsv_field(buf, cap, &o, s->ext, '\n');
  }
  return (int)o;
}

bool kv_get(const char *body, const char *key, char *out, size_t cap);

static bool config_path(char *buf, size_t cap) {
  const char *home = getenv("HOME");
  if (!home || !*home) return false;
  snprintf(buf, cap, "%s/.config/padwave/config", home);
  return true;
}

int app::config_text(char *buf, size_t cap) {
  size_t o = 0;
  if (cap) buf[0] = 0;
  kv_add(buf, cap, &o, "config.idle_timeout_ms", "%d", this->idle_timeout_ms);
  kv_add(buf, cap, &o, "config.idle_warn_grace_ms", "%d", this->idle_warn_grace_ms);
  kv_add(buf, cap, &o, "config.trigger_effect_amplitude", "%d", this->trigger_effect_amplitude);
  kv_add(buf, cap, &o, "config.trigger_effect_position", "%d", this->trigger_effect_position);
  kv_add(buf, cap, &o, "config.trigger_effect_frequency", "%d", this->trigger_effect_frequency);
  kv_add(buf, cap, &o, "config.trigger_effects", "%s", this->trigger_effects ? "true" : "false");
  return (int)o;
}

void app::config_load() {
  char path[512];
  if (!config_path(path, sizeof(path))) return;
  FILE *f = fopen(path, "r");
  if (!f) return;

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *value = eq + 1;
    char *nl = strchr(value, '\n');
    if (nl) *nl = 0;

    if (strcmp(line, "idle_timeout_ms") == 0) {
      long v = atol(value);
      this->idle_timeout_ms = (int)(v < 1000 ? 1000 : (v > 86400000 ? 86400000 : v));
    } else if (strcmp(line, "idle_warn_grace_ms") == 0) {
      long v = atol(value);
      this->idle_warn_grace_ms = (int)(v < 0 ? 0 : (v > 600000 ? 600000 : v));
    } else if (strcmp(line, "trigger_effect_amplitude") == 0) {
      int v = atoi(value);
      this->trigger_effect_amplitude = v < 0 ? 0 : (v > 8 ? 8 : v);
    } else if (strcmp(line, "trigger_effect_position") == 0) {
      int v = atoi(value);
      this->trigger_effect_position = v < 0 ? 0 : (v > 9 ? 9 : v);
    } else if (strcmp(line, "trigger_effect_frequency") == 0) {
      int v = atoi(value);
      this->trigger_effect_frequency = v < 1 ? 1 : (v > 255 ? 255 : v);
    } else if (strcmp(line, "trigger_effects") == 0) {
      this->trigger_effects = strcmp(value, "true") == 0 || atoi(value) != 0;
    }
  }
  fclose(f);
}

void app::config_save() {
  char path[512];
  if (!config_path(path, sizeof(path))) return;

  char dir[512];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = 0;
    mkdir(dir, 0755);
  }

  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "idle_timeout_ms=%d\n", this->idle_timeout_ms);
  fprintf(f, "idle_warn_grace_ms=%d\n", this->idle_warn_grace_ms);
  fprintf(f, "trigger_effect_amplitude=%d\n", this->trigger_effect_amplitude);
  fprintf(f, "trigger_effect_position=%d\n", this->trigger_effect_position);
  fprintf(f, "trigger_effect_frequency=%d\n", this->trigger_effect_frequency);
  fprintf(f, "trigger_effects=%s\n", this->trigger_effects ? "true" : "false");
  fclose(f);
}

void app::config_set(const char *body) {
  char value[64];

  if (kv_get(body, "idle_timeout_ms", value, sizeof(value))) {
    long v = atol(value);
    this->idle_timeout_ms = (int)(v < 1000 ? 1000 : (v > 86400000 ? 86400000 : v));
  }
  if (kv_get(body, "idle_warn_grace_ms", value, sizeof(value))) {
    long v = atol(value);
    this->idle_warn_grace_ms = (int)(v < 0 ? 0 : (v > 600000 ? 600000 : v));
  }
  if (kv_get(body, "trigger_effect_amplitude", value, sizeof(value))) {
    int v = atoi(value);
    this->trigger_effect_amplitude = v < 0 ? 0 : (v > 8 ? 8 : v);
  }
  if (kv_get(body, "trigger_effect_position", value, sizeof(value))) {
    int v = atoi(value);
    this->trigger_effect_position = v < 0 ? 0 : (v > 9 ? 9 : v);
  }
  if (kv_get(body, "trigger_effect_frequency", value, sizeof(value))) {
    int v = atoi(value);
    this->trigger_effect_frequency = v < 1 ? 1 : (v > 255 ? 255 : v);
  }
  if (kv_get(body, "trigger_effects", value, sizeof(value)))
    this->trigger_effects = strcmp(value, "true") == 0 || atoi(value) != 0;

  config_save();
}

int bindings_text(char *buf, size_t cap) {
  size_t o = 0;
  if (cap) buf[0] = 0;
  kv_text(buf, cap, &o, "bindings.circle", "play");
  kv_text(buf, cap, &o, "bindings.create", "previous");
  kv_text(buf, cap, &o, "bindings.fn1", "toggle_pause");
  kv_text(buf, cap, &o, "bindings.fn2", "toggle_mute");
  kv_text(buf, cap, &o, "bindings.l1", "previous");
  kv_text(buf, cap, &o, "bindings.l3", "next_random");
  kv_text(buf, cap, &o, "bindings.left_paddle", "previous");
  kv_text(buf, cap, &o, "bindings.mute", "toggle_mute");
  kv_text(buf, cap, &o, "bindings.options", "next_random");
  kv_text(buf, cap, &o, "bindings.r1", "next_random");
  kv_text(buf, cap, &o, "bindings.r3", "previous");
  kv_text(buf, cap, &o, "bindings.right_paddle", "next_random");
  kv_text(buf, cap, &o, "bindings.square", "shuffle_game");
  kv_text(buf, cap, &o, "bindings.touchpad", "toggle_mute");
  kv_text(buf, cap, &o, "bindings.triangle", "toggle_pause");
  return (int)o;
}

// Commands arrive as the same key=value lines the panel parses back.
bool kv_get(const char *body, const char *key, char *out, size_t cap) {
  if (!body || cap == 0) return false;
  size_t klen = strlen(key);
  for (const char *p = body; *p;) {
    const char *eol = strchr(p, '\n');
    size_t n = eol ? (size_t)(eol - p) : strlen(p);
    if (n > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
      const char *v = p + klen + 1;
      size_t vn = n - klen - 1;
      if (vn > 0 && v[vn - 1] == '\r') vn--;
      if (vn >= cap) vn = cap - 1;
      memcpy(out, v, vn);
      out[vn] = 0;
      return true;
    }
    if (!eol) break;
    p = eol + 1;
  }
  return false;
}

long kv_number(const char *body, const char *key, long fallback) {
  char tmp[32];
  if (!kv_get(body, key, tmp, sizeof(tmp))) return fallback;
  return atol(tmp);
}

void app::output_fields(const char *fields) {
  if (!this->player) return;
  char line[1200];
  snprintf(line, sizeof(line), "fields %s\n", fields);
  player_send(line);
}

// The panel's lab: translate a kind + params into the player's `fields`
// control report. The player owns the output hidraw and does the write.
int app::output(const char *body, char *out, size_t out_cap) {
  char kind[32];
  if (!kv_get(body, "kind", kind, sizeof(kind)))
    return snprintf(out, out_cap, "ok=false\nerror=missing kind\n");
  if (!this->player) return snprintf(out, out_cap, "ok=false\nerror=no player\n");

  char fields[1024];

  if (strcmp(kind, "lightbar") == 0) {
    char rgb[64] = "0,0,0";
    int r = 0, g = 0, b = 0;
    kv_get(body, "rgb", rgb, sizeof(rgb));
    sscanf(rgb, "%d,%d,%d", &r, &g, &b);
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    snprintf(fields, sizeof(fields),
             "valid_flag1=lightbar;lightbar_red=%d;lightbar_green=%d;lightbar_blue=%d", r, g, b);
    output_fields(fields);
    return snprintf(out, out_cap, "ok=true\nnote=lightbar %d,%d,%d\n", r, g, b);
  }

  if (strcmp(kind, "lightbar_reset") == 0) {
    output_fields("valid_flag1=reset_lights");
    return snprintf(out, out_cap, "ok=true\nnote=lightbar reset\n");
  }

  if (strcmp(kind, "lightbar_fade") == 0) {
    output_fields("valid_flag1=lightbar;valid_flag2=color_fade");
    return snprintf(out, out_cap, "ok=true\nnote=fade\n");
  }

  if (strcmp(kind, "lightbar_setup") == 0) {
    int v = (int)kv_number(body, "value", 0);
    snprintf(fields, sizeof(fields), "valid_flag1=lightbar;lightbar_setup=%d", v & 0xFF);
    output_fields(fields);
    return snprintf(out, out_cap, "ok=true\nnote=setup %d\n", v & 0xFF);
  }

  if (strcmp(kind, "lightbar_brightness") == 0) {
    int v = (int)kv_number(body, "value", 0);
    snprintf(fields, sizeof(fields), "valid_flag2=brightness;led_brightness=%d", v & 0xFF);
    output_fields(fields);
    return snprintf(out, out_cap, "ok=true\nnote=brightness %d\n", v & 0xFF);
  }

  if (strcmp(kind, "player_leds") == 0) {
    int mask = (int)kv_number(body, "mask", 0);
    snprintf(fields, sizeof(fields), "valid_flag1=player_leds;player_leds=%d", mask & 0x1F);
    output_fields(fields);
    return snprintf(out, out_cap, "ok=true\nnote=player leds %d\n", mask & 0x1F);
  }

  if (strcmp(kind, "power_save") == 0) {
    char bits[64] = "";
    kv_get(body, "bits", bits, sizeof(bits));
    int mask = 0;
    char *save = nullptr;
    for (char *tok = strtok_r(bits, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
      int bit = atoi(tok);
      if (bit >= 0 && bit < 8) mask |= 1 << bit;
    }
    snprintf(fields, sizeof(fields), "valid_flag1=power_save;power_save_control=%d", mask);
    output_fields(fields);
    return snprintf(out, out_cap, "ok=true\nnote=power save 0x%02x\n", mask);
  }

  if (strcmp(kind, "rumble") == 0) {
    int left = (int)kv_number(body, "left", 0);
    int right = (int)kv_number(body, "right", 0);
    long ms = kv_number(body, "ms", 0);
    left = left < 0 ? 0 : (left > 255 ? 255 : left);
    right = right < 0 ? 0 : (right > 255 ? 255 : right);
    snprintf(fields, sizeof(fields), "valid_flag0=rumble;motor_left=%d;motor_right=%d", left, right);
    output_fields(fields);
    this->rumble_left = left;
    this->rumble_right = right;
    this->rumble_until = ms > 0 ? now_ms() + ms : 0;
    return snprintf(out, out_cap, "ok=true\nnote=rumble %d/%d\n", left, right);
  }

  if (strcmp(kind, "trigger") == 0) {
    char side[16] = "right";
    char effect[16] = "vibration";
    kv_get(body, "side", side, sizeof(side));
    kv_get(body, "effect", effect, sizeof(effect));
    int position = (int)kv_number(body, "position", 2);
    int amplitude = (int)kv_number(body, "amplitude", 6);
    int frequency = (int)kv_number(body, "frequency", 25);
    long ms = kv_number(body, "ms", 0);

    int mode = 0x26;
    if (strcmp(effect, "off") == 0) mode = 0x05;
    else if (strcmp(effect, "feedback") == 0) mode = 0x21;

    int params[10] = {0};
    if (mode != 0x05) {
      params[0] = position;
      params[1] = amplitude;
      params[9] = frequency;
    }

    bool left = strcmp(side, "left") == 0;
    const char *flag = left ? "left_trigger" : "right_trigger";
    const char *prefix = left ? "left" : "right";
    snprintf(fields, sizeof(fields),
             "valid_flag0=%s;%s_trigger_mode=0x%02x;%s_trigger_params=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
             flag, prefix, mode, prefix, params[0], params[1], params[2], params[3], params[4],
             params[5], params[6], params[7], params[8], params[9]);
    output_fields(fields);
    this->trigger_side = left ? 1 : 2;
    this->trigger_until = (ms > 0 && mode != 0x05) ? now_ms() + ms : 0;
    return snprintf(out, out_cap, "ok=true\nnote=trigger %s 0x%02x\n", side, mode);
  }

  if (strcmp(kind, "fields") == 0) {
    if (!kv_get(body, "fields", fields, sizeof(fields)))
      return snprintf(out, out_cap, "ok=false\nerror=missing fields\n");
    output_fields(fields);
    return snprintf(out, out_cap, "ok=true\nnote=fields\n");
  }

  if (strcmp(kind, "mic_monitor") == 0)
    return snprintf(out, out_cap, "ok=false\nerror=mic monitor is not implemented\n");

  return snprintf(out, out_cap, "ok=false\nerror=unknown kind\n");
}

// Deadlines are served from the player timer (10 ms) or the app tick.
void app::service_output() {
  if (!this->player) return;
  long now = now_ms();
  if (this->rumble_until && now >= this->rumble_until) {
    output_fields("valid_flag0=rumble;motor_left=0;motor_right=0");
    this->rumble_until = 0;
  }
  if (this->trigger_until && now >= this->trigger_until) {
    const char *flag = this->trigger_side == 1 ? "left_trigger" : "right_trigger";
    const char *prefix = this->trigger_side == 1 ? "left" : "right";
    char fields[192];
    snprintf(fields, sizeof(fields),
             "valid_flag0=%s;%s_trigger_mode=0x05;%s_trigger_params=0,0,0,0,0,0,0,0,0,0", flag,
             prefix, prefix);
    output_fields(fields);
    this->trigger_until = 0;
    this->trigger_side = 0;
  }
}

int app::command(const char *action, const char *body, size_t body_len, char *out, size_t out_cap) {
  (void)body_len;

  if (strcmp(action, "next") == 0) next();
  else if (strcmp(action, "previous") == 0) previous();
  else if (strcmp(action, "shuffle_game") == 0) shuffle_game();
  else if (strcmp(action, "pause") == 0) toggle_pause();
  else if (strcmp(action, "mute") == 0) set_volume(this->muted ? this->volume : 0);
  else if (strcmp(action, "disconnect") == 0) {
    this->playing = false;
    this->paused = true;
    player_send("pause 1\n");
  } else if (strcmp(action, "volume") == 0) {
    set_volume((int)kv_number(body, "value", this->volume));
  } else if (strcmp(action, "seek") == 0) {
    seek(this->pos_ms + kv_number(body, "delta", 0) * 1000);
  } else if (strcmp(action, "seek_to") == 0) {
    seek(kv_number(body, "position", this->pos_ms / 1000) * 1000);
  } else if (strcmp(action, "play_track") == 0) {
    char id[64];
    if (kv_get(body, "id", id, sizeof(id))) {
      int idx = find_id(id);
      if (idx >= 0) play(idx, 0);
    }
  } else {
    return snprintf(out, out_cap, "ok=false\nerror=unknown action\n");
  }

  snprintf(this->last_action, sizeof(this->last_action), "%s", action);
  return snprintf(out, out_cap, "ok=true\n");
}

int app::handle_command(const char *body, size_t len, char *out, size_t cap) {
  char action[64];
  if (!kv_get(body, "action", action, sizeof(action)))
    return snprintf(out, cap, "ok=false\nerror=missing action\n");
  return command(action, body, len, out, cap);
}

bool app::owns_fd(int fd) {
  return fd >= 0 && (fd == this->player_timer_fd || fd == this->input_fd);
}

bool app::init() {
  signal(SIGPIPE, SIG_IGN);

  const char *roots = getenv("PADWAVE_ROOTS");
  if (!roots || !*roots) roots = getenv("HOME") ? "$HOME/Music" : "/music";
  char rootbuf[2048];
  snprintf(rootbuf, sizeof(rootbuf), "%s", roots);

  char *save = nullptr;
  for (char *dir = strtok_r(rootbuf, ":", &save); dir; dir = strtok_r(nullptr, ":", &save)) {
    if (strstr(dir, "$HOME")) {
      const char *home = getenv("HOME");
      char expanded[2048];
      snprintf(expanded, sizeof(expanded), "%s%s", home ? home : "", dir + 5);
      scan_dir(expanded, "");
    } else {
      scan_dir(dir, "");
    }
  }

  qsort(this->stations, (size_t)this->count, sizeof(station), cmp_station);
  srand((unsigned)time(nullptr) ^ (unsigned)getpid());
  this->player = nullptr;
  this->input = nullptr;
  this->player_timer_fd = this->input_fd = -1;
  config_load();
  set_volume(70);
  return true;
}

void app::shutdown() {
  if (this->player) {
    player_send("quit\n");
    ::player_destroy(this->player);
    this->player = nullptr;
  }
  this->player_timer_fd = -1;
  input_stop();
}

void app::tick() {
  service_output();
  if (this->eof) {
    this->eof = false;
    next();
  }
  if (!this->player) {
    player_start();
    if (this->player && this->current < 0 && this->count > 0) {
      int start = rand() % this->count;
      play(start, 0);
    }
  }
  if (!this->input && (int)now_ms() >= this->reconnect_at) input_start();
}

int app::fds(int *fds, int max) {
  int n = 0;
  if (this->player_timer_fd >= 0 && n < max) fds[n++] = this->player_timer_fd;
  if (this->input_fd >= 0 && n < max) fds[n++] = this->input_fd;
  return n;
}

void app::handle_fd(int fd) {
  if (this->player && fd == this->player_timer_fd) {
    player_step(this->player);
    service_output();
    return;
  }
  if (this->input && fd == this->input_fd) {
    uint8_t buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      input_feed(this->input, buf, (size_t)n);
    } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
      input_stop();
      this->reconnect_at = (int)(now_ms() + 1500);
    }
  }
}

/* ---- opaque API used by the server --------------------------------------- */

void app_tick(app *a) { a->tick(); }
int app_fds(app *a, int *fds, int max) { return a->fds(fds, max); }
void app_handle_fd(app *a, int fd) { a->handle_fd(fd); }
int app_status_text(app *a, char *buf, size_t cap) { return a->status_text(buf, cap); }
int app_front_text(app *a, char *buf, size_t cap, unsigned long requests, long uptime) {
  return a->front_text(buf, cap, requests, uptime);
}
int app_command(app *a, const char *action, const char *body, size_t len, char *out, size_t cap) {
  return a->command(action, body, len, out, cap);
}
int app_handle_command(app *a, const char *body, size_t len, char *out, size_t cap) {
  return a->handle_command(body, len, out, cap);
}
bool app_owns_fd(app *a, int fd) { return a->owns_fd(fd); }// padwave serve — the C++ web front.
//
// One epoll loop, one `struct server` holding the connection table, the epoll
// bookkeeping, the request counter and the embedded panel. The app is the only
// backend: PADWAVE_STANDALONE=1 creates it in-process and the routes read its
// state. /api/events and /api/front/events are chunked SSE from the same
// 500 ms timer that ticks the app, with a 64 KiB buffer per connection and
// backpressure when the client cannot keep up.
//
//   padwave serve [port]      default 8787
//
// The panel's hot routes (/api/status, /api/front, /api/command) speak
// key=value lines; the structural ones (/api/library, /api/config,
// /api/schema) stay JSON.
//
// The UI is #embed on compilers that have it (Clang); GCC 12 gets a generated
// padwave_ui.hh from install.sh. Either way the binary needs no files.


#if defined(__has_embed)
#if __has_embed("index.html")
static const unsigned char kIndexHtml[] = {
#embed "index.html"
};
static const size_t kIndexHtmlSize = sizeof(kIndexHtml);
#else
#include "padwave_ui.hh"
#endif
#else
#include "padwave_ui.hh"
#endif

namespace {

constexpr int kMaxConns = 64;
constexpr size_t kRequestMax = 8192;
constexpr size_t kBufferMax = 65536;
constexpr int kMaxFds = 1024;

struct conn {
  int fd = -1;  // client
  char in[kRequestMax];
  size_t in_len = 0;
  char out[kBufferMax];
  size_t out_len = 0;
  size_t out_off = 0;
  bool static_reply = false;
  bool front_sse = false;
  bool status_sse = false;
  bool client_blocked = false;
};

// The whole server state: the connection table, the epoll bookkeeping, the
// app it serves and the counters/static response the routes read. No route
// needs a file-scope variable; server_main passes this down.
struct server {
  conn conns[kMaxConns];
  conn *owner[kMaxFds];
  int epoll_fd = -1;
  bool standalone = false;
  int timer_fd = -1;
  int64_t start_ms = 0;
  unsigned long requests = 0;
  char index_response[70000];
  size_t index_len = 0;
  app *a = nullptr;
};

int64_t monotonic_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int make_static(char *dst, size_t cap, const char *type, const char *body, size_t body_len) {
  int n = snprintf(dst, cap,
                   "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                   "Connection: close\r\nCache-Control: no-cache\r\n\r\n",
                   type, body_len);
  if (n < 0 || (size_t)n + body_len > cap) return -1;
  memcpy(dst + n, body, body_len);
  return n + (int)body_len;
}

int front_text(server *s, char *buf, size_t cap);

void close_conn(server *s, conn *c) {
  if (c->fd >= 0) {
    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, c->fd, nullptr);
    if (c->fd < kMaxFds) s->owner[c->fd] = nullptr;
    close(c->fd);
  }
  *c = conn{};
}

bool add_fd(server *s, int fd, uint32_t events, conn *owner) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
  if (fd < kMaxFds) s->owner[fd] = owner;
  return true;
}

bool mod_fd(server *s, int fd, uint32_t events) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  return epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void read_client(server *s, conn *c) {
  if (c->in_len >= kRequestMax - 1) {
    close_conn(s, c);
    return;
  }
  ssize_t n = read(c->fd, c->in + c->in_len, kRequestMax - 1 - c->in_len);
  if (n == 0) {
    close_conn(s, c);
    return;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    close_conn(s, c);
    return;
  }
  c->in_len += (size_t)n;
  c->in[c->in_len] = 0;

  char *end = strstr(c->in, "\r\n\r\n");
  if (!end) {
    if (c->in_len >= kRequestMax - 1) close_conn(s, c);
    return;
  }

  size_t head_len = (size_t)(end - c->in) + 4;
  size_t body_len = 0;
  const char *cl = strcasestr(c->in, "Content-Length:");
  if (cl && cl < end) body_len = (size_t)strtoul(cl + 15, nullptr, 10);
  if (head_len + body_len > kRequestMax) {
    close_conn(s, c);
    return;
  }
  if (c->in_len < head_len + body_len) return;  // body still arriving

  char method[8] = {0}, path[1024] = {0};
  if (sscanf(c->in, "%7s %1023s", method, path) != 2) {
    close_conn(s, c);
    return;
  }

  s->requests++;

  if (s->standalone && strcmp(path, "/api/status") == 0) {
    static char body[8192];
    int len = app_status_text(s->a, body, sizeof(body));
    c->static_reply = true;
    c->out_len =
        (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", body, (size_t)len);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strcmp(path, "/api/events") == 0) {
    c->status_sse = true;
    c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                  "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                                  "Connection: keep-alive\r\n\r\n");
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strcmp(path, "/api/library") == 0) {
    char *body = (char *)malloc(65536);
    int len = body ? s->a->library_text(body, 65536) : 0;
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8",
                                     body ? body : "", body ? (size_t)len : 0);
    free(body);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strcmp(path, "/api/config") == 0) {
    if (strcmp(method, "POST") == 0) {
      s->a->config_set(c->in + head_len);
      static const char ok[] = "ok=true\n";
      c->static_reply = true;
      c->out_len =
          (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
      mod_fd(s, c->fd, EPOLLOUT);
      return;
    }
    char body[1024];
    int len = s->a->config_text(body, sizeof(body));
    c->static_reply = true;
    c->out_len =
        (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", body, (size_t)len);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strcmp(path, "/api/schema") == 0) {
    char body[1024];
    int len = bindings_text(body, sizeof(body));
    c->static_reply = true;
    c->out_len =
        (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", body, (size_t)len);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strcmp(path, "/api/command") == 0) {
    if (strcmp(method, "POST") != 0) {
      c->static_reply = true;
      c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                    "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n"
                                    "Connection: close\r\n\r\n");
      mod_fd(s, c->fd, EPOLLOUT);
      return;
    }
    char result[512];
    int len = app_handle_command(s->a, c->in + head_len, body_len, result, sizeof(result));
    c->static_reply = true;
    c->out_len =
        (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", result, (size_t)len);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strcmp(path, "/api/output") == 0) {
    if (strcmp(method, "POST") != 0) {
      c->static_reply = true;
      c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                    "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n"
                                    "Connection: close\r\n\r\n");
      mod_fd(s, c->fd, EPOLLOUT);
      return;
    }
    char result[256];
    int len = s->a->output(c->in + head_len, result, sizeof(result));
    c->static_reply = true;
    c->out_len =
        (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", result, (size_t)len);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strcmp(path, "/api/debug/trigger") == 0) {
    char side[16] = "right";
    char tmp[16];
    if (kv_get(c->in + head_len, "side", tmp, sizeof(tmp)))
      snprintf(side, sizeof(side), "%s", tmp);
    long ms = kv_number(c->in + head_len, "ms", 700);
    char request[192];
    snprintf(request, sizeof(request),
             "kind=trigger\nside=%s\neffect=vibration\nposition=2\namplitude=6\n"
             "frequency=25\nms=%ld",
             side, ms);
    char result[256];
    int len = s->a->output(request, result, sizeof(result));
    c->static_reply = true;
    c->out_len =
        (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", result, (size_t)len);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strcmp(path, "/api/mic/reset") == 0) {
    static const char body[] = "ok=true\n";
    c->static_reply = true;
    c->out_len =
        (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", body, sizeof(body) - 1);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strcmp(path, "/api/reload") == 0) {
    static const char body[] = "ok=true\nreloaded=0\n";
    c->static_reply = true;
    c->out_len =
        (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", body, sizeof(body) - 1);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (s->standalone && strncmp(path, "/api/", 5) == 0 && strcmp(path, "/api/front") != 0 && strcmp(path, "/api/front/events") != 0) {
    static const char body[] = "{\"ok\":false,\"error\":\"not implemented\"}";
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body,
                                     sizeof(body) - 1);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (strcmp(path, "/api/front/events") == 0) {
    c->front_sse = true;
    c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                  "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                                  "Connection: keep-alive\r\n\r\n");
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  if (strcmp(path, "/api/front") == 0) {
    char body[1024];
    int len = front_text(s, body, sizeof(body));
    c->static_reply = true;
    c->out_len =
        (size_t)make_static(c->out, sizeof(c->out), "text/plain; charset=utf-8", body, (size_t)len);
    mod_fd(s, c->fd, EPOLLOUT);
    return;
  }

  c->static_reply = true;
  if (strcmp(path, "/health") == 0) {
    static const char body[] = "{\"ok\":true}\n";
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body,
                                     sizeof(body) - 1);
  } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
    if (s->index_len <= sizeof(c->out)) {
      memcpy(c->out, s->index_response, s->index_len);
      c->out_len = s->index_len;
    } else {
      c->out_len = 0;
    }
  } else {
    static const char body[] = "not found\n";
    int n = snprintf(c->out, sizeof(c->out),
                     "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     sizeof(body) - 1, body);
    c->out_len = (size_t)n;
  }
  mod_fd(s, c->fd, EPOLLOUT);
}

void flush_client(server *s, conn *c) {
  while (c->out_off < c->out_len) {
    ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        c->client_blocked = true;
        mod_fd(s, c->fd, EPOLLOUT);
        return;
      }
      close_conn(s, c);
      return;
    }
    c->out_off += (size_t)n;
  }
  c->out_off = c->out_len = 0;
  c->client_blocked = false;

  if (c->front_sse || c->status_sse) {
    mod_fd(s, c->fd, 0);
    return;
  }

  if (c->static_reply) close_conn(s, c);
}

int front_text(server *s, char *buf, size_t cap) {
  return app_front_text(s->a, buf, cap, s->requests,
                        (long)((monotonic_ms() - s->start_ms) / 1000));
}

void append_sse(server *s, conn *c, const char *body, size_t n) {
  if (c->out_off > 0) {
    if (c->out_off < c->out_len) memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
    c->out_len -= c->out_off;
    c->out_off = 0;
  }
  if (n == 0) return;

  // Every key=value line needs its own data: field; a line without one is
  // dropped by the event source. Count the exact payload, then write it.
  size_t payload = 1;  // the blank line that ends the event
  for (size_t pos = 0; pos < n;) {
    size_t end = pos;
    while (end < n && body[end] != '\n') end++;
    payload += 6 + (end - pos) + 1;
    pos = end < n ? end + 1 : end;
  }

  char *chunk = (char *)malloc(payload + 48);
  if (!chunk) return;

  size_t pos = 0;
  int m = snprintf(chunk, 48, "%zx\r\n", payload);
  size_t o = (m > 0) ? (size_t)m : 0;

  while (pos < n) {
    size_t end = pos;
    while (end < n && body[end] != '\n') end++;
    memcpy(chunk + o, "data: ", 6);
    o += 6;
    memcpy(chunk + o, body + pos, end - pos);
    o += end - pos;
    chunk[o++] = '\n';
    pos = end < n ? end + 1 : end;
  }
  chunk[o++] = '\n';
  memcpy(chunk + o, "\r\n", 2);
  o += 2;

  if (c->out_len + o <= sizeof(c->out)) {
    memcpy(c->out + c->out_len, chunk, o);
    c->out_len += o;
    mod_fd(s, c->fd, EPOLLOUT);
  }
  free(chunk);
}

void append_status_event(server *s, conn *c) {
  char body[8192];
  int n = app_status_text(s->a, body, sizeof(body));
  if (n > 0) append_sse(s, c, body, (size_t)n);
}

void append_front_event(server *s, conn *c) {
  char body[1024];
  int n = front_text(s, body, sizeof(body));
  if (n > 0) append_sse(s, c, body, (size_t)n);
}

} // namespace

int server_main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8787;
  static server storage{};  // ~5 MB of connection buffers; zero-initialized BSS
  server *s = &storage;
  app a{};                  // the app lives on the stack; no heap ownership

  int n = snprintf(nullptr, 0,
                   "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                   "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",
                   kIndexHtmlSize);
  if ((size_t)n + kIndexHtmlSize >= sizeof(s->index_response)) {
    fprintf(stderr, "padwave serve: UI too large for the static buffer\n");
    return 1;
  }
  snprintf(s->index_response, sizeof(s->index_response),
           "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
           "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",
           kIndexHtmlSize);
  memcpy(s->index_response + n, kIndexHtml, kIndexHtmlSize);
  s->index_len = (size_t)n + kIndexHtmlSize;

  int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd < 0) {
    perror("socket");
    return 1;
  }
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int flags = fcntl(listen_fd, F_GETFL, 0);
  fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(listen_fd);
    return 1;
  }
  if (listen(listen_fd, 64) < 0) {
    perror("listen");
    close(listen_fd);
    return 1;
  }

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigprocmask(SIG_BLOCK, &mask, nullptr);
  int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

  s->standalone = getenv("PADWAVE_STANDALONE") != nullptr;
  if (s->standalone) {
    if (a.init()) s->a = &a;
    else s->standalone = false;
  }
  s->start_ms = monotonic_ms();

  s->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (s->timer_fd >= 0) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_nsec = 500000000;
    its.it_value = its.it_interval;
    timerfd_settime(s->timer_fd, 0, &its, nullptr);
  }

  s->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (s->epoll_fd < 0) {
    perror("epoll_create1");
    return 1;
  }
  add_fd(s, listen_fd, EPOLLIN, nullptr);
  if (signal_fd >= 0) add_fd(s, signal_fd, EPOLLIN, nullptr);
  if (s->timer_fd >= 0) add_fd(s, s->timer_fd, EPOLLIN, nullptr);
  if (s->standalone) {
    int fds[4];
    int count = app_fds(s->a, fds, 4);
    for (int i = 0; i < count; i++) add_fd(s, fds[i], EPOLLIN, nullptr);
  }

  fprintf(stderr, "padwave serve: http://0.0.0.0:%d (%zu byte UI)\n", port, kIndexHtmlSize);

  struct epoll_event events[64];
  bool running = true;
  while (running) {
    int ready = epoll_wait(s->epoll_fd, events, 64, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < ready; i++) {
      int fd = events[i].data.fd;
      if (fd == listen_fd) {
        int listener = fd;
        for (;;) {
          int client = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (client < 0) break;
          conn *slot = nullptr;
          for (int j = 0; j < kMaxConns; j++)
            if (s->conns[j].fd < 0) {
              slot = &s->conns[j];
              break;
            }
          if (!slot) {
            close(client);
            continue;
          }
          *slot = conn{};
          slot->fd = client;
          add_fd(s, client, EPOLLIN, slot);
        }
        continue;
      }
      if (fd == s->timer_fd) {
        uint64_t expirations = 0;
        if (read(s->timer_fd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
          if (s->standalone) {
            app_tick(s->a);
            int fds[4];
            int count = app_fds(s->a, fds, 4);
            for (int i = 0; i < count; i++) add_fd(s, fds[i], EPOLLIN, nullptr);
          }
          for (int j = 0; j < kMaxConns; j++) {
            if (s->conns[j].fd < 0) continue;
            if (s->conns[j].front_sse) append_front_event(s, &s->conns[j]);
            if (s->conns[j].status_sse) append_status_event(s, &s->conns[j]);
          }
        }
        continue;
      }
      if (fd == signal_fd) {
        struct signalfd_siginfo info;
        if (read(signal_fd, &info, sizeof(info)) == sizeof(info)) running = false;
        continue;
      }

      if (s->standalone && app_owns_fd(s->a, fd)) {
        app_handle_fd(s->a, fd);
        continue;
      }

      conn *c = (fd >= 0 && fd < kMaxFds) ? s->owner[fd] : nullptr;
      if (!c || fd != c->fd) continue;

      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        close_conn(s, c);
        continue;
      }
      if (events[i].events & EPOLLIN) read_client(s, c);
      if (c->fd >= 0 && (events[i].events & EPOLLOUT)) flush_client(s, c);
    }
  }

  for (int i = 0; i < kMaxConns; i++) close_conn(s, &s->conns[i]);
  if (s->timer_fd >= 0) close(s->timer_fd);
  if (s->a) s->a->shutdown();
  close(s->epoll_fd);
  if (signal_fd >= 0) close(signal_fd);
  close(listen_fd);
  return 0;
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