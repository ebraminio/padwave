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
#include <stdarg.h>
#include <stdbool.h>
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

namespace padwave_status {

static constexpr const char *kPath = "/tmp/padwave-status.sock";
static constexpr const char *kPlayerPath = "/tmp/padwave-player.sock";

static inline int sender_open() {
  return socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

static inline void send_line(int fd, const char *line) {
  if (fd < 0) return;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", kPath);
  (void)sendto(fd, line, strlen(line), 0, (struct sockaddr *)&addr, sizeof(addr));
}

} // namespace padwave_status

// Entry points and in-process engines. The service calls the player/input
// engines directly from its epoll loop; the *_main wrappers keep the debug
// modes working.

int native_main(int argc, char **argv);
int input_main(int argc, char **argv);
int opusdec_main(int argc, char **argv);
int server_main(int argc, char **argv);

void *player_create(const char *node, bool speaker);
void player_set_sink(void *handle, void (*sink)(const char *, void *), void *ud);
int player_timer_fd(void *handle);
int player_bus_fd(void *handle);
void player_step(void *handle);
void player_on_bus(void *handle);
void player_command(void *handle, const char *line);
void player_destroy(void *handle);

void *input_create(const char *node);
void input_set_sink(void *handle, void (*sink)(const char *, void *), void *ud);
int input_fd(void *handle);
int input_feed(void *handle, const uint8_t *buf, size_t len);
void input_destroy(void *handle);

// Opaque handle to the standalone app. The state and methods live in
// padwave_app.cc; the server owns one instance and passes it to the callbacks
// as userdata.

struct app;

struct app *app_create();
void app_destroy(struct app *a);
void app_tick(struct app *a);
int app_fds(struct app *a, int *fds, int max);
void app_handle_fd(struct app *a, int fd);
int app_status_json(struct app *a, char *buf, size_t cap);
int app_library_json(struct app *a, char *buf, size_t cap);
int app_config_json(char *buf, size_t cap);
int app_bindings_json(char *buf, size_t cap);
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



static void (*g_player_sink)(const char *, void *) = nullptr;
static void *g_player_sink_ud = nullptr;
static char g_player_emit_buf[8192];
static size_t g_player_emit_len = 0;

static void player_emit(const char *fmt, ...) {
  char tmp[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  size_t n = strlen(tmp);
  if (g_player_emit_len + n >= sizeof(g_player_emit_buf)) g_player_emit_len = 0;
  memcpy(g_player_emit_buf + g_player_emit_len, tmp, n);
  g_player_emit_len += n;
  g_player_emit_buf[g_player_emit_len] = 0;

  char *start = g_player_emit_buf;
  char *nl;
  while ((nl = strchr(start, '\n'))) {
    *nl = 0;
    if (g_player_sink) g_player_sink(start, g_player_sink_ud);
    else puts(start);
    start = nl + 1;
  }
  size_t left = strlen(start);
  memmove(g_player_emit_buf, start, left + 1);
  g_player_emit_len = left;
}

#define printf(...) player_emit(__VA_ARGS__)

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

  printf("track %s\n", pl->path);
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
    printf("track %s\n", pl->path);
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
  int bus_fd;
  int status_fd;
  int64_t last_status;
};

void player_set_sink(void *handle, void (*sink)(const char *, void *), void *ud) {
  (void)handle;
  g_player_sink = sink;
  g_player_sink_ud = ud;
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

  h->status_fd = padwave_status::sender_open();

  h->bus_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (h->bus_fd >= 0) {
    struct sockaddr_un cmd_addr;
    memset(&cmd_addr, 0, sizeof(cmd_addr));
    cmd_addr.sun_family = AF_UNIX;
    snprintf(cmd_addr.sun_path, sizeof(cmd_addr.sun_path), "%s", padwave_status::kPlayerPath);
    unlink(padwave_status::kPlayerPath);
    if (bind(h->bus_fd, (struct sockaddr *)&cmd_addr, sizeof(cmd_addr)) != 0) {
      close(h->bus_fd);
      h->bus_fd = -1;
    }
  }

  h->last_status = 0;
  return h;
}

int player_timer_fd(void *handle) { return handle ? ((player_handle *)handle)->tfd : -1; }
int player_bus_fd(void *handle) { return handle ? ((player_handle *)handle)->bus_fd : -1; }

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

void player_on_bus(void *handle) {
  player_handle *h = (player_handle *)handle;
  if (!h || h->bus_fd < 0) return;

  char cmd[1200];
  ssize_t n = recv(h->bus_fd, cmd, sizeof(cmd) - 1, 0);
  if (n <= 0) return;
  cmd[n] = 0;
  player_command(h, cmd);
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
    int64_t pos = pl.base_ms + pl.consumed * 1000 / RATE;
    printf("pos %ld\ndur %ld\nsent %ld\nunderruns %ld\npushed %ld\n", (long)pos,
           (long)pl.cur.duration_ms, (long)pl.sent, (long)pl.underruns, (long)pl.pushed);

    char status_line[160];
    snprintf(status_line, sizeof(status_line), "player %ld %ld %ld %ld %d", (long)pos,
             (long)pl.cur.duration_ms, (long)pl.sent, (long)pl.underruns,
             pl.paused ? 1 : 0);
    padwave_status::send_line(h->status_fd, status_line);
    if (pl.eof && pl.tail == pl.head && pl.nxt.fmt == NULL && !pl.has_next) printf("eof\n");
  }
}

void player_destroy(void *handle) {
  player_handle *h = (player_handle *)handle;
  if (!h) return;

  struct player &pl = h->pl;
  close_file(&pl);
  close_next(&pl);
  if (h->bus_fd >= 0) {
    close(h->bus_fd);
    unlink(padwave_status::kPlayerPath);
  }
  if (pl.hidraw >= 0) close(pl.hidraw);
  if (pl.enc) opus_encoder_destroy(pl.enc);
  if (pl.pkt) av_packet_free(&pl.pkt);
  if (pl.frame) av_frame_free(&pl.frame);
  if (h->tfd >= 0) close(h->tfd);
  if (h->status_fd >= 0) close(h->status_fd);
  free(h);
}

int native_main(int argc, char **argv) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  void *h = player_create(argc > 1 ? argv[1] : nullptr,
                          argc > 2 && strcmp(argv[2], "speaker") == 0);
  if (!h) return 1;

  int tfd = player_timer_fd(h);
  int bfd = player_bus_fd(h);
  char line[1200];
  int line_len = 0;

  while (running) {
    struct pollfd fds[3];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = tfd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    fds[2].fd = bfd;
    fds[2].events = POLLIN;
    fds[2].revents = 0;
    int ready = poll(fds, 3, 20);
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
    if (bfd >= 0 && (fds[2].revents & POLLIN)) player_on_bus(h);
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
// A separate process from the player on purpose: reading input is independent
// of whether audio is playing, and the player should not own the input fd just
// because it happens to be the process that also writes 0x36. This CLI is the
// input mode: it parses 0x31 (Bluetooth) and
// 0x01 (USB) with InputReport/InputBody from padwave_schemas.hh and prints one
// line per state report, plus "mic <hex>" for the Opus frames that share 0x31.
//
//   padwave input [hidraw-node]
//   padwave input --hex 31c1...        one captured report, decode and exit
//   padwave input --file /path         a file of raw reports (testing)
//
// Fields are the names the Elixir side already speaks: buttons are the
// InputBody::Buttons enumerators, dpad is the InputBody::Dpad name, and the
// rest is straight from the body. battery/charging keep Frame.battery/1 and
// Frame.charging/1 semantics (10% steps, nibble 0/1/2).


static void (*g_input_sink)(const char *, void *) = nullptr;
static void *g_input_sink_ud = nullptr;
static char g_input_emit_buf[4096];
static size_t g_input_emit_len = 0;

static void input_emit(const char *fmt, ...) {
  char tmp[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  size_t n = strlen(tmp);
  if (g_input_emit_len + n >= sizeof(g_input_emit_buf)) g_input_emit_len = 0;
  memcpy(g_input_emit_buf + g_input_emit_len, tmp, n);
  g_input_emit_len += n;
  g_input_emit_buf[g_input_emit_len] = 0;

  char *start = g_input_emit_buf;
  char *nl;
  while ((nl = strchr(start, '\n'))) {
    *nl = 0;
    if (g_input_sink) g_input_sink(start, g_input_sink_ud);
    else puts(start);
    start = nl + 1;
  }
  size_t left = strlen(start);
  memmove(g_input_emit_buf, start, left + 1);
  g_input_emit_len = left;
}

#define printf(...) input_emit(__VA_ARGS__)

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

void print_buttons(uint32_t buttons) {
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
  bool first = true;
  for (auto &n : names) {
    if (!(buttons & n.bit)) continue;
    printf("%s%s", first ? "" : ",", n.name);
    first = false;
  }
}

/* -- d-pad steering -------------------------------------------------------
 * The d-pad's timing lives here now: a tap seeks 6 s, a hold ramps through
 * 10/20/40/60 s at 420 ms, then repeats the last step at 130 ms. The actions
 * are printed as lines and the app only executes them. */
/* -- buttons and triggers -------------------------------------------------
 * The static bindings table moves here with the d-pad. The radio only executes
 * the actions; the UI's bindings map is documentation, not logic. */
int64_t input_now_ms();
void emit_action(const char *line);
void emit_event(const char *line);

struct actions_state {
  uint32_t prev_buttons = 0;
  int64_t last_l2 = 0;
  int64_t last_r2 = 0;
  int64_t ps_at = 0;
  bool ps_armed = false;
};

actions_state g_actions;

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

void track_buttons(uint32_t buttons) {
  uint32_t pressed = buttons & ~g_actions.prev_buttons;
  g_actions.prev_buttons = buttons;

  for (auto &b : kBindings)
    if (pressed & b.bit) emit_action(b.action);

  if (pressed & padwave::InputBody::Ps) {
    g_actions.ps_armed = true;
    g_actions.ps_at = input_now_ms();
  }

  if (!(buttons & padwave::InputBody::Ps)) {
    g_actions.ps_armed = false;
  } else if (g_actions.ps_armed && input_now_ms() - g_actions.ps_at >= 2000) {
    emit_action("suspend");
    g_actions.ps_armed = false;
  }
}

void track_triggers(int l2, int r2) {
  int64_t now = input_now_ms();
  if (l2 >= 30 && now - g_actions.last_l2 >= 110) {
    int step = l2 >= 220 ? 4 : (l2 >= 150 ? 2 : 1);
    printf("action volume %d\n", -step);
    fflush(stdout);
    g_actions.last_l2 = now;
  }
  if (r2 >= 30 && now - g_actions.last_r2 >= 110) {
    int step = r2 >= 220 ? 4 : (r2 >= 150 ? 2 : 1);
    printf("action volume %d\n", step);
    fflush(stdout);
    g_actions.last_r2 = now;
  }
}

/* -- sensors and the remaining controller events -------------------------
 * Activity/wake (with the analog cap), shake, the 250 ms jack debounce and the
 * trigger actuator's pressed/released edges. The radio still executes the
 * actions; these are announcements, not decisions. */
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

sensors_state g_sensors;

void track_sensors(const padwave::InputBody &b) {
  int64_t now = input_now_ms();
  bool jack = (b.status1.get() & padwave::InputBody::Headset) != 0;
  int l2 = b.l2.get(), r2 = b.r2.get();
  int lx = b.lx.get(), ly = b.ly.get(), rx = b.rx.get(), ry = b.ry.get();
  uint32_t buttons = b.buttons;
  uint8_t dpad = (uint8_t)(buttons & 0x0F);
  bool first = !g_sensors.has_prev;

  bool real = false, analog = false;
  if (!first) {
    real = buttons != g_sensors.prev_buttons || dpad != g_sensors.prev_dpad ||
           jack != g_sensors.prev_jack || abs(l2 - g_sensors.prev_l2) > 10 ||
           abs(r2 - g_sensors.prev_r2) > 10;
    analog = abs(lx - g_sensors.prev_lx) > 24 || abs(ly - g_sensors.prev_ly) > 24 ||
             abs(rx - g_sensors.prev_rx) > 24 || abs(ry - g_sensors.prev_ry) > 24;
  }

  double g = sqrt((double)b.accel_x.get() * b.accel_x.get() +
                  (double)b.accel_y.get() * b.accel_y.get() +
                  (double)b.accel_z.get() * b.accel_z.get()) /
             8192.0;
  if (g >= 2.25 && now - g_sensors.last_shake > 1500) {
    emit_event("shake");
    g_sensors.last_shake = now;
    real = true;
  }

  if (real || analog) {
    bool capped = analog && !real && g_sensors.analog_resets >= 6;
    if (!capped && now - g_sensors.last_activity > 900) {
      emit_event("activity");
      g_sensors.last_activity = now;
      if (analog) g_sensors.analog_resets++;
      else g_sensors.analog_resets = 0;
    } else if (real) {
      g_sensors.analog_resets = 0;
    }
  }

  if (first) {
    g_sensors.stable_jack = jack ? 1 : 0;
  } else if (jack == (g_sensors.stable_jack != 0)) {
    g_sensors.pending_jack = -1;
    g_sensors.jack_since = 0;
  } else if (g_sensors.pending_jack != (jack ? 1 : 0) || g_sensors.jack_since == 0) {
    g_sensors.pending_jack = jack ? 1 : 0;
    g_sensors.jack_since = now;
  } else if (now - g_sensors.jack_since >= 250) {
    emit_action(jack ? "jack_plugged" : "jack_unplugged");
    g_sensors.stable_jack = jack ? 1 : 0;
    g_sensors.pending_jack = -1;
    g_sensors.jack_since = 0;
  }

  if (!g_sensors.l2_down && l2 >= 30) {
    emit_event("trigger l pressed");
    g_sensors.l2_down = true;
  } else if (g_sensors.l2_down && l2 < 15) {
    emit_event("trigger l released");
    g_sensors.l2_down = false;
  }
  if (!g_sensors.r2_down && r2 >= 30) {
    emit_event("trigger r pressed");
    g_sensors.r2_down = true;
  } else if (g_sensors.r2_down && r2 < 15) {
    emit_event("trigger r released");
    g_sensors.r2_down = false;
  }

  g_sensors.has_prev = true;
  g_sensors.prev_buttons = buttons;
  g_sensors.prev_dpad = dpad;
  g_sensors.prev_lx = lx;
  g_sensors.prev_ly = ly;
  g_sensors.prev_rx = rx;
  g_sensors.prev_ry = ry;
  g_sensors.prev_l2 = l2;
  g_sensors.prev_r2 = r2;
  g_sensors.prev_jack = jack;
}

struct dpad_state {
  uint8_t prev = 0xFF;
  int hold_dir = 0;
  int hold_step = 0;
  int64_t hold_at = 0;
};

dpad_state g_dpad;

int64_t input_now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void emit_action(const char *line) {
  printf("action %s\n", line);
  fflush(stdout);
}

void emit_event(const char *line) {
  printf("event %s\n", line);
  fflush(stdout);
}

void track_dpad(uint32_t buttons) {
  static const int steps[] = {10, 20, 40, 60};
  static const int intervals[] = {320, 260, 190, 130};

  uint8_t dpad = (uint8_t)(buttons & 0x0F);
  int64_t now = input_now_ms();

  if (dpad != g_dpad.prev) {
    g_dpad.prev = dpad;
    g_dpad.hold_dir = 0;

    switch (dpad) {
      case padwave::InputBody::Left:
        emit_action("seek -6");
        g_dpad.hold_dir = -1;
        g_dpad.hold_step = 0;
        g_dpad.hold_at = now + 420;
        break;
      case padwave::InputBody::Right:
        emit_action("seek 6");
        g_dpad.hold_dir = 1;
        g_dpad.hold_step = 0;
        g_dpad.hold_at = now + 420;
        break;
      case padwave::InputBody::Up:
        emit_action("next_random");
        break;
      case padwave::InputBody::Down:
        emit_action("previous");
        break;
      default:
        break;
    }
    return;
  }

  if (g_dpad.hold_dir == 0 || now < g_dpad.hold_at) return;

  int index = g_dpad.hold_step < 4 ? g_dpad.hold_step : 3;
  printf("action seek %d\n", g_dpad.hold_dir * steps[index]);
  fflush(stdout);
  g_dpad.hold_step = index + 1;
  g_dpad.hold_at = now + intervals[index];
}

void print_frame(const padwave::InputBody &b, const uint8_t *raw, size_t len) {
  uint32_t buttons = b.buttons;
  int battery = (b.status0 & 0x0F) * 10 + 5;
  if (battery > 100) battery = 100;
  printf("frame seq=%u lx=%u ly=%u rx=%u ry=%u l2=%u r2=%u dpad=%s buttons=", (unsigned)b.seq.get(),
         (unsigned)b.lx.get(), (unsigned)b.ly.get(), (unsigned)b.rx.get(), (unsigned)b.ry.get(),
         (unsigned)b.l2.get(), (unsigned)b.r2.get(), dpad_name(buttons & 0x0F));
  print_buttons(buttons);
  printf(" battery=%d charging=%u jack=%d mic=%d temp=%u", battery, (unsigned)(b.status0.get() >> 4),
         (b.status1.get() & padwave::InputBody::Headset) ? 1 : 0,
         (b.status1.get() & padwave::InputBody::Microphone) ? 1 : 0, (unsigned)b.temperature.get());
  printf(" gx=%d gy=%d gz=%d ax=%d ay=%d az=%d", b.gyro_x.get(), b.gyro_y.get(), b.gyro_z.get(),
         b.accel_x.get(), b.accel_y.get(), b.accel_z.get());
  printf(" tfl=%u tfr=%u", (unsigned)b.trigger_feedback_l.get(),
         (unsigned)b.trigger_feedback_r.get());
  printf(" raw=");
  for (size_t i = 0; i < len; i++) printf("%02x", raw[i]);
  printf(" t1=%02x%02x%02x%02x t2=%02x%02x%02x%02x\n", (unsigned)b.touch1[0].get(),
         (unsigned)b.touch1[1].get(), (unsigned)b.touch1[2].get(), (unsigned)b.touch1[3].get(),
         (unsigned)b.touch2[0].get(), (unsigned)b.touch2[1].get(), (unsigned)b.touch2[2].get(),
         (unsigned)b.touch2[3].get());
  fflush(stdout);

  track_dpad(b.buttons);
  track_buttons(b.buttons);
  track_triggers(b.l2, b.r2);
  track_sensors(b);

  static int64_t last_status = 0;
  static int status_fd = padwave_status::sender_open();
  int64_t now = input_now_ms();
  if (now - last_status >= 100) {
    char status_line[200];
    snprintf(status_line, sizeof(status_line), "input %u %u %u %u %u %u %u %u %d", (unsigned)b.lx.get(),
             (unsigned)b.ly.get(), (unsigned)b.rx.get(), (unsigned)b.ry.get(), (unsigned)b.l2.get(),
             (unsigned)b.r2.get(), buttons, (unsigned)(b.status0.get() & 0x0F) * 10 + 5,
             (b.status1.get() & padwave::InputBody::Headset) ? 1 : 0);
    padwave_status::send_line(status_fd, status_line);
    last_status = now;
  }
}

// One report: 0x31 Bluetooth (78), 0x01 USB (64), or a 0x31 microphone frame.
void handle_report(const uint8_t *buf, size_t len) {
  if (len > 7 && padwave::MicFrame::is_frame(buf, len)) {
    size_t payload_len = 0;
    const uint8_t *payload = padwave::MicFrame::payload(buf, len, &payload_len);
    printf("mic ");
    for (size_t i = 0; i < payload_len; i++) printf("%02x", payload[i]);
    printf("\n");
    fflush(stdout);
    return;
  }

  if (buf[0] == padwave::PacketStateBluetooth && len >= padwave::InputReport::Size) {
    const padwave::InputReport *r = (const padwave::InputReport *)buf;
    if (!r->valid()) return;
    print_frame(r->body, buf, padwave::InputReport::Size);
    return;
  }

  if (buf[0] == padwave::PacketInputUsb && len >= padwave::InputReport::UsbSize)
    print_frame(*(const padwave::InputBody *)(buf + 1), buf, padwave::InputReport::UsbSize);
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

struct input_handle {
  int fd;
};

void input_set_sink(void *handle, void (*sink)(const char *, void *), void *ud) {
  (void)handle;
  g_input_sink = sink;
  g_input_sink_ud = ud;
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
  h->fd = fd;
  return h;
}

int input_fd(void *handle) { return handle ? ((input_handle *)handle)->fd : -1; }

int input_feed(void *handle, const uint8_t *buf, size_t len) {
  if (!handle) return -1;
  handle_report(buf, len);
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

  if (argc > 1 && strcmp(argv[1], "--hex") == 0 && argc > 2) {
    size_t len = 0;
    if (!hex_decode(argv[2], buf, &len)) return 2;
    if (len > 1 && (buf[0] == 0xA1 || buf[0] == 0xA2)) {
      memmove(buf, buf + 1, len - 1);
      len--;
    }
    handle_report(buf, len);
    return 0;
  }

  if (argc > 1 && strcmp(argv[1], "--file") == 0 && argc > 2) {
    int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 2;
    for (;;) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) break;
      handle_report(buf, (size_t)n);
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
#undef printf
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
  int player_bus_fd = -1;
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
  unsigned button_mask = 0;
  int temp = 0;
  int ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  char last_action[64] = "";
  char raw[400] = "";

  int reconnect_at = 0;
  long idle_since = 0;

  void add_station(const char *path, const char *game);
  void scan_dir(const char *dir, const char *game);
  int find_station(const char *path);
  int find_id(const char *id);
  void push_history(int index);
  void player_send(const char *line);
  void player_start();
  void input_start();
  void input_stop();
  void player_line(const char *line);
  void play(int index, long start_ms);
  void next();
  void previous();
  void shuffle_game();
  void toggle_pause();
  void set_volume(int v);
  void seek(long ms);
  void controller_from_frame(const char *line);
  void handle_action(const char *action);
  void input_line(const char *line);
  void status_radio(char *buf, size_t cap);
  void status_controller(char *buf, size_t cap);
  int status_json(char *buf, size_t cap);
  int library_json(char *buf, size_t cap);
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

void json_escape(const char *in, char *out, size_t cap) {
  size_t o = 0;
  for (const char *p = in; *p && o + 2 < cap; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = (char)c;
    } else if (c < 0x20) {
      o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
    } else {
      out[o++] = (char)c;
    }
  }
  out[o] = 0;
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

void player_sink(const char *line, void *ud) { ((app *)ud)->player_line(line); }
void input_sink(const char *line, void *ud) { ((app *)ud)->input_line(line); }

void app::player_send(const char *line) {
  if (this->player) ::player_command(this->player, line);
}

void app::player_start() {
  if (this->player) return;
  this->player = ::player_create(nullptr, false);
  if (!this->player) return;
  ::player_set_sink(this->player, player_sink, this);
  this->player_timer_fd = ::player_timer_fd(this->player);
  this->player_bus_fd = ::player_bus_fd(this->player);
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
  ::input_set_sink(this->input, input_sink, this);
  this->input_fd = ::input_fd(this->input);
}

void app::input_stop() {
  if (this->input) {
    ::input_destroy(this->input);
    this->input = nullptr;
  }
  this->input_fd = -1;
  this->controller = false;
}

void app::player_line(const char *line) {
  if (strncmp(line, "pos ", 4) == 0) {
    this->pos_ms = atol(line + 4);
  } else if (strncmp(line, "dur ", 4) == 0) {
    this->dur_ms = atol(line + 4);
  } else if (strncmp(line, "sent ", 5) == 0) {
    this->sent = atol(line + 5);
  } else if (strncmp(line, "underruns ", 10) == 0) {
    this->underruns = atol(line + 10);
  } else if (strncmp(line, "track ", 6) == 0) {
    int idx = find_station(line + 6);
    if (idx >= 0) {
      if (this->current >= 0 && this->current != idx) push_history(this->current);
      this->current = idx;
      this->paused = false;
      this->playing = true;
    }
  } else if (strcmp(line, "eof") == 0) {
    this->eof = true;
    this->playing = false;
  }
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

void app::controller_from_frame(const char *line) {
  // frame seq=.. lx=.. ... dpad=.. buttons=.. battery=.. charging=.. jack=..
  this->controller = true;
  char buttons[256] = "";
  char dpad[16] = "none";
  const char *p = line;
  while ((p = strchr(p, ' ')) != nullptr) {
    p++;
    if (strncmp(p, "lx=", 3) == 0) this->lx = atoi(p + 3);
    else if (strncmp(p, "ly=", 3) == 0) this->ly = atoi(p + 3);
    else if (strncmp(p, "rx=", 3) == 0) this->rx = atoi(p + 3);
    else if (strncmp(p, "ry=", 3) == 0) this->ry = atoi(p + 3);
    else if (strncmp(p, "l2=", 3) == 0) this->l2 = atoi(p + 3);
    else if (strncmp(p, "r2=", 3) == 0) this->r2 = atoi(p + 3);
    else if (strncmp(p, "battery=", 8) == 0) this->battery = atoi(p + 8);
    else if (strncmp(p, "charging=", 9) == 0) {
      int c = atoi(p + 9);
      snprintf(this->charging, sizeof(this->charging), "%s",
               c == 1 ? "charging" : c == 2 ? "full" : "discharging");
    } else if (strncmp(p, "jack=", 5) == 0) this->jack = atoi(p + 5) != 0;
    else if (strncmp(p, "temp=", 5) == 0) this->temp = atoi(p + 5);
    else if (strncmp(p, "ax=", 3) == 0) this->ax = atoi(p + 3);
    else if (strncmp(p, "ay=", 3) == 0) this->ay = atoi(p + 3);
    else if (strncmp(p, "az=", 3) == 0) this->az = atoi(p + 3);
    else if (strncmp(p, "gx=", 3) == 0) this->gx = atoi(p + 3);
    else if (strncmp(p, "gy=", 3) == 0) this->gy = atoi(p + 3);
    else if (strncmp(p, "gz=", 3) == 0) this->gz = atoi(p + 3);
    else if (strncmp(p, "dpad=", 5) == 0) {
      size_t n = strcspn(p + 5, " ");
      if (n >= sizeof(dpad)) n = sizeof(dpad) - 1;
      memcpy(dpad, p + 5, n);
      dpad[n] = 0;
    } else if (strncmp(p, "buttons=", 8) == 0) {
      size_t n = strcspn(p + 8, " ");
      if (n >= sizeof(buttons)) n = sizeof(buttons) - 1;
      memcpy(buttons, p + 8, n);
      buttons[n] = 0;
    } else if (strncmp(p, "raw=", 4) == 0) {
      size_t n = strcspn(p + 4, " ");
      if (n >= sizeof(this->raw)) n = sizeof(this->raw) - 1;
      memcpy(this->raw, p + 4, n);
      this->raw[n] = 0;
    }
  }
  snprintf(this->dpad, sizeof(this->dpad), "%s", dpad);
  snprintf(this->buttons, sizeof(this->buttons), "%s", buttons);
}

void app::handle_action(const char *action) {
  if (strcmp(action, "next") == 0) next();
  else if (strcmp(action, "previous") == 0) previous();
  else if (strcmp(action, "shuffle_game") == 0) shuffle_game();
  else if (strcmp(action, "pause") == 0) toggle_pause();
  else if (strcmp(action, "mute") == 0) set_volume(this->muted ? this->volume : 0);
  else if (strcmp(action, "disconnect") == 0) {
    this->playing = false;
    this->paused = true;
    player_send("pause 1\n");
  }
  snprintf(this->last_action, sizeof(this->last_action), "%s", action);
}

void app::input_line(const char *line) {
  if (strncmp(line, "frame ", 6) == 0) {
    controller_from_frame(line);
  } else if (strncmp(line, "action ", 7) == 0) {
    char action[64];
    snprintf(action, sizeof(action), "%s", line + 7);
    char *space = strchr(action, ' ');
    if (space && strncmp(action, "volume ", 7) == 0) {
      int delta = atoi(space + 1);
      set_volume(this->volume + delta);
      snprintf(this->last_action, sizeof(this->last_action), "volume");
    } else if (space && strncmp(action, "seek ", 5) == 0) {
      seek(this->pos_ms + atol(space + 1) * 1000);
      snprintf(this->last_action, sizeof(this->last_action), "seek");
    } else {
      if (space) *space = 0;
      handle_action(action);
    }
  } else if (strncmp(line, "down", 4) == 0) {
    input_stop();
    this->reconnect_at = (int)(now_ms() + 1500);
  }
}

void app::status_radio(char *buf, size_t cap) {
  const station *s = this->current >= 0 ? &this->stations[this->current] : nullptr;
  char title[2 * kMaxTitle], game[2 * kMaxGame], path[2 * kMaxPath];
  json_escape(s ? s->title : "", title, sizeof(title));
  json_escape(s ? s->game : "", game, sizeof(game));
  json_escape(s ? s->path : "", path, sizeof(path));

  char cur[3 * kMaxPath] = "null";
  if (s)
    snprintf(cur, sizeof(cur),
             "{\"id\":\"%s\",\"title\":\"%s\",\"game\":\"%s\",\"path\":\"%s\",\"ext\":\"%s\"}",
             s->id, title, game, path, s->ext);

  snprintf(buf, cap,
           "{\"playing\":%d,\"paused\":%d,\"position\":%.1f,\"duration\":%.1f,"
           "\"volume\":%d,\"muted\":%d,\"idle_warning\":0,\"suspended\":0,"
           "\"suspended_by\":null,\"resume_pending\":0,\"paused_by_jack\":0,"
           "\"applied_volume\":%d,\"fade_percent\":100,\"current\":%s,\"history\":[]}",
           this->playing ? 1 : 0, this->paused ? 1 : 0, this->pos_ms / 1000.0, this->dur_ms / 1000.0, this->volume,
           this->muted ? 1 : 0, this->volume, cur);
}

void app::status_controller(char *buf, size_t cap) {
  char buttons[512];
  json_escape(this->buttons, buttons, sizeof(buttons));
  char arr[600];
  size_t o = 0;
  o += (size_t)snprintf(arr + o, sizeof(arr) - o, "[");
  bool first = true;
  char *save = nullptr;
  for (char *tok = strtok_r(buttons, ",", &save); tok && o + 4 < sizeof(arr);
       tok = strtok_r(nullptr, ",", &save)) {
    o += (size_t)snprintf(arr + o, sizeof(arr) - o, "%s\"%s\"", first ? "" : ",", tok);
    first = false;
  }
  snprintf(arr + o, sizeof(arr) - o, "]");

  char node[64] = "";
  int idx = find_dualsense(node, sizeof(node));
  char raw[420];
  json_escape(this->raw, raw, sizeof(raw));

  snprintf(buf, cap,
           "{\"connected\":%d,\"bus\":\"bluetooth\",\"name\":\"DualSense\",\"mac\":\"\","
           "\"model\":\"dualsense\",\"node\":\"%s\",\"battery\":%d,\"charging\":\"%s\","
           "\"jack\":%d,\"buttons\":%s,\"dpad\":\"%s\",\"l2\":%d,\"r2\":%d,"
           "\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"temperature\":%d,"
           "\"accel\":[%d,%d,%d],\"gyro\":[%d,%d,%d],\"raw\":\"%s\","
           "\"last_action\":\"%s\",\"fields\":{},\"touches\":[]}",
           this->controller ? 1 : 0, idx >= 0 ? node : "", this->battery >= 0 ? this->battery : 0, this->charging, this->jack ? 1 : 0, arr, this->dpad,
           this->l2, this->r2, this->lx, this->ly, this->rx, this->ry, this->temp, this->ax, this->ay, this->az, this->gx, this->gy, this->gz, raw,
           this->last_action);
}


int app::status_json(char *buf, size_t cap) {
  char radio[4096], controller[4096];
  status_radio(radio, sizeof(radio));
  status_controller(controller, sizeof(controller));

  return snprintf(buf, cap,
                  "{\"radio\":%s,\"session\":{\"phase\":\"active\",\"idle_seconds\":0,"
                  "\"warning_in\":900,\"shutdown_in\":0},\"audio\":{\"present\":false,"
                  "\"sink\":\"\",\"state\":null,\"streams\":[]},\"hid\":{\"writes\":0,"
                  "\"errors\":0,\"recent\":[],\"power_save\":null,\"player_leds\":null},"
                  "\"led\":{\"mode\":\"idle\",\"custom\":null,\"bright\":false,"
                  "\"brightness\":null,\"setup\":null,\"painted\":null},"
                  "\"mic\":{\"decoder\":\"\",\"running\":false,\"active\":false,\"packets\":0,"
                  "\"frames\":0,\"errors\":0,\"rms\":0,\"peak\":0,\"level\":0,\"samples\":0,"
                  "\"last_at\":null,\"enabled\":false,\"monitor\":false},"
                  "\"player\":{\"port\":%d,\"path\":\"\",\"position\":%.1f,\"duration\":%.1f,"
                  "\"paused\":%d,\"volume\":%d,\"sent\":%ld,\"underruns\":%ld,\"errors\":0,"
                  "\"eof\":%d},\"controller\":%s,\"bindings\":{},\"config\":{},\"now\":%ld}",
                  radio, this->player ? 1 : 0, this->pos_ms / 1000.0, this->dur_ms / 1000.0,
                  this->paused ? 1 : 0, this->volume, this->sent, this->underruns, this->eof ? 1 : 0, controller,
                  (long)(time(nullptr)));
}

int app::library_json(char *buf, size_t cap) {
  int n = snprintf(buf, cap, "{\"tracks\":[");
  for (int i = 0; i < this->count && n > 0 && (size_t)n < cap; i++) {
    const station *s = &this->stations[i];
    char title[2 * kMaxTitle], game[2 * kMaxGame], path[2 * kMaxPath];
    json_escape(s->title, title, sizeof(title));
    json_escape(s->game, game, sizeof(game));
    json_escape(s->path, path, sizeof(path));
    n += snprintf(buf + n, cap - (size_t)n,
                  "%s{\"id\":\"%s\",\"title\":\"%s\",\"game\":\"%s\",\"path\":\"%s\",\"ext\":\"%s\"}",
                  i ? "," : "", s->id, title, game, path, s->ext);
  }
  if (n > 0 && (size_t)n < cap) n += snprintf(buf + n, cap - (size_t)n, "]}");
  return n;
}

int app_config_json(char *buf, size_t cap) {
  return snprintf(buf, cap,
                  "{\"crossfade_ms\":1500,\"idle_timeout_ms\":900000,\"idle_warn_grace_ms\":10000,"
                  "\"poweroff_when_idle_ms\":60000,\"idle_fade_floor\":10,\"idle_fade_ms\":10000,"
                  "\"analog_wake_cap\":6,\"analog_wake_delta\":24,\"charging_leds\":true,"
                  "\"charging_leds_ms\":3000,\"trigger_deadzone\":30,\"trigger_effects\":true,"
                  "\"trigger_effect_position\":2,\"trigger_effect_amplitude\":6,"
                  "\"trigger_effect_frequency\":25,\"seek_crossfade_threshold\":20,"
                  "\"seek_crossfade_ms\":600,\"fade_in_ms\":900,\"fade_out_ms\":450,"
                  "\"volume_fade_ms\":180,\"lightbar_enabled\":true,\"shake_g_force\":2.25}");
}

int app_bindings_json(char *buf, size_t cap) {
  return snprintf(buf, cap,
                  "{\"circle\":\"play\",\"create\":\"previous\",\"fn1\":\"toggle_pause\","
                  "\"fn2\":\"toggle_mute\",\"l1\":\"previous\",\"l3\":\"next_random\","
                  "\"left_paddle\":\"previous\",\"mute\":\"toggle_mute\",\"options\":\"next_random\","
                  "\"r1\":\"next_random\",\"r3\":\"previous\",\"right_paddle\":\"next_random\","
                  "\"square\":\"shuffle_game\",\"touchpad\":\"toggle_mute\","
                  "\"triangle\":\"toggle_pause\"}");
}

long json_number(const char *body, const char *key, long fallback) {
  if (!body) return fallback;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(body, pattern);
  if (!p) return fallback;
  p = strchr(p + strlen(pattern), ':');
  if (!p) return fallback;
  return atol(p + 1);
}

bool json_string(const char *body, const char *key, char *out, size_t cap) {
  if (!body) return false;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(body, pattern);
  if (!p) return false;
  p = strchr(p + strlen(pattern), ':');
  if (!p) return false;
  p = strchr(p, '"');
  if (!p) return false;
  p++;
  const char *e = strchr(p, '"');
  if (!e) return false;
  size_t n = (size_t)(e - p);
  if (n >= cap) n = cap - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
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
    set_volume((int)json_number(body, "value", this->volume));
  } else if (strcmp(action, "seek") == 0) {
    seek(this->pos_ms + json_number(body, "delta", 0) * 1000);
  } else if (strcmp(action, "seek_to") == 0) {
    seek(json_number(body, "position", this->pos_ms / 1000) * 1000);
  } else if (strcmp(action, "play_track") == 0) {
    char id[64];
    if (json_string(body, "id", id, sizeof(id))) {
      int idx = find_id(id);
      if (idx >= 0) play(idx, 0);
    }
  } else {
    return snprintf(out, out_cap, "{\"ok\":false,\"error\":\"unknown action\"}");
  }

  snprintf(this->last_action, sizeof(this->last_action), "%s", action);
  return snprintf(out, out_cap, "{\"ok\":true}");
}

int app::handle_command(const char *body, size_t len, char *out, size_t cap) {
  char action[64];
  if (!json_string(body, "action", action, sizeof(action)))
    return snprintf(out, cap, "{\"ok\":false,\"error\":\"missing action\"}");
  return command(action, body, len, out, cap);
}

bool app::owns_fd(int fd) {
  return fd >= 0 && (fd == this->player_timer_fd || fd == this->player_bus_fd || fd == this->input_fd);
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
  this->player_timer_fd = this->player_bus_fd = this->input_fd = -1;
  set_volume(70);
  return true;
}

void app::shutdown() {
  if (this->player) {
    player_send("quit\n");
    ::player_destroy(this->player);
    this->player = nullptr;
  }
  this->player_timer_fd = this->player_bus_fd = -1;
  input_stop();
}

void app::tick() {
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
  if (this->player_bus_fd >= 0 && n < max) fds[n++] = this->player_bus_fd;
  if (this->input_fd >= 0 && n < max) fds[n++] = this->input_fd;
  return n;
}

void app::handle_fd(int fd) {
  if (this->player && fd == this->player_timer_fd) {
    player_step(this->player);
    return;
  }
  if (this->player && fd == this->player_bus_fd) {
    player_on_bus(this->player);
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

app *app_create() {
  app *a = new app();
  if (!a->init()) {
    delete a;
    return nullptr;
  }
  return a;
}

void app_destroy(app *a) {
  if (!a) return;
  a->shutdown();
  delete a;
}

void app_tick(app *a) { a->tick(); }
int app_fds(app *a, int *fds, int max) { return a->fds(fds, max); }
void app_handle_fd(app *a, int fd) { a->handle_fd(fd); }
int app_status_json(app *a, char *buf, size_t cap) { return a->status_json(buf, cap); }
int app_library_json(app *a, char *buf, size_t cap) { return a->library_json(buf, cap); }
int app_command(app *a, const char *action, const char *body, size_t len, char *out, size_t cap) {
  return a->command(action, body, len, out, cap);
}
int app_handle_command(app *a, const char *body, size_t len, char *out, size_t cap) {
  return a->handle_command(body, len, out, cap);
}
bool app_owns_fd(app *a, int fd) { return a->owns_fd(fd); }// padwave serve — the C++ web front.
//
// A single epoll loop serving the panel from the binary and proxying /api/* to
// the standalone radio; while PADWAVE_STANDALONE is unset it can still
// proxy to a legacy backend. The proxy is streaming, so /api/events (SSE) works:
// bytes are relayed as they arrive, with a 64 KiB per-connection buffer and
// backpressure when the client cannot keep up.
//
//   padwave serve [port] [backend-port]      default 8787 8787
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
  int fd = -1;              // client
  int up = -1;              // backend, when proxying
  char in[kRequestMax];
  size_t in_len = 0;
  char up_req[kRequestMax];
  size_t up_len = 0;
  size_t up_off = 0;
  char out[kBufferMax];
  size_t out_len = 0;
  size_t out_off = 0;
  bool up_sent = false;
  bool static_reply = false;
  bool front_sse = false;
  bool status_sse = false;
  bool client_blocked = false;
};

conn g_conns[kMaxConns];
conn *g_owner[kMaxFds];

int g_epoll = -1;
int g_backend_port = 8787;
int g_status_fd = -1;
bool g_standalone = false;
int g_timer_fd = -1;
int64_t g_start_ms = 0;
unsigned long g_requests = 0;

struct player_status {
  bool valid = false;
  long pos = 0, dur = 0, sent = 0, underruns = 0;
  int paused = 0;
};

struct input_status {
  bool valid = false;
  int lx = 0, ly = 0, rx = 0, ry = 0, l2 = 0, r2 = 0, battery = 0, jack = 0;
  unsigned buttons = 0;
};

player_status g_player;
input_status g_input;

int64_t monotonic_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void parse_status(const char *line) {
  if (strncmp(line, "player ", 7) == 0) {
    if (sscanf(line + 7, "%ld %ld %ld %ld %d", &g_player.pos, &g_player.dur, &g_player.sent,
               &g_player.underruns, &g_player.paused) == 5)
      g_player.valid = true;
  } else if (strncmp(line, "input ", 6) == 0) {
    unsigned lx, ly, rx, ry, l2, r2, buttons;
    int battery, jack;
    if (sscanf(line + 6, "%u %u %u %u %u %u %u %d %d", &lx, &ly, &rx, &ry, &l2, &r2, &buttons,
               &battery, &jack) == 9) {
      g_input = input_status{};
      g_input.valid = true;
      g_input.lx = (int)lx;
      g_input.ly = (int)ly;
      g_input.rx = (int)rx;
      g_input.ry = (int)ry;
      g_input.l2 = (int)l2;
      g_input.r2 = (int)r2;
      g_input.buttons = buttons;
      g_input.battery = battery;
      g_input.jack = jack;
    }
  }
}
char g_index_response[70000];
size_t g_index_len = 0;

int make_static(char *dst, size_t cap, const char *type, const char *body, size_t body_len) {
  int n = snprintf(dst, cap,
                   "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                   "Connection: close\r\nCache-Control: no-cache\r\n\r\n",
                   type, body_len);
  if (n < 0 || (size_t)n + body_len > cap) return -1;
  memcpy(dst + n, body, body_len);
  return n + (int)body_len;
}

int front_json(char *buf, size_t cap);

void close_conn(conn *c) {
  if (c->fd >= 0) {
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, nullptr);
    if (c->fd < kMaxFds) g_owner[c->fd] = nullptr;
    close(c->fd);
  }
  if (c->up >= 0) {
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->up, nullptr);
    if (c->up < kMaxFds) g_owner[c->up] = nullptr;
    close(c->up);
  }
  *c = conn{};
}

bool add_fd(int fd, uint32_t events, conn *owner) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  if (epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
  if (fd < kMaxFds) g_owner[fd] = owner;
  return true;
}

bool mod_fd(int fd, uint32_t events) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  return epoll_ctl(g_epoll, EPOLL_CTL_MOD, fd, &ev) == 0;
}

// Copy the request line and all headers except Connection/Keep-Alive, then a
// Connection: close, so the backend closes after the response (SSE stays open
// because the client detaches, not the backend).
size_t build_upstream_request(conn *c, const char *headers_end, size_t body_len) {
  char *dst = c->up_req;
  size_t cap = sizeof(c->up_req);
  size_t used = 0;

  const char *line_end = strstr(c->in, "\r\n");
  size_t line_len = line_end ? (size_t)(line_end - c->in) + 2 : 0;
  if (line_len == 0 || line_len > cap) return 0;
  memcpy(dst, c->in, line_len);
  used = line_len;

  const char *p = c->in + line_len;
  while (p < headers_end) {
    const char *eol = strstr(p, "\r\n");
    if (!eol || eol == p) break;
    size_t len = (size_t)(eol - p) + 2;
    bool drop = strncasecmp(p, "Connection:", 11) == 0 || strncasecmp(p, "Keep-Alive:", 11) == 0 ||
                strncasecmp(p, "Proxy-Connection:", 17) == 0;
    if (!drop) {
      if (used + len > cap) return 0;
      memcpy(dst + used, p, len);
      used += len;
    }
    p = eol + 2;
  }

  static const char close_hdr[] = "Connection: close\r\n\r\n";
  if (used + sizeof(close_hdr) - 1 + body_len > cap) return 0;
  memcpy(dst + used, close_hdr, sizeof(close_hdr) - 1);
  used += sizeof(close_hdr) - 1;
  if (body_len) {
    memcpy(dst + used, headers_end + 4, body_len);
    used += body_len;
  }
  return used;
}

void start_proxy(conn *c, const char *headers_end, size_t body_len) {
  c->up_len = build_upstream_request(c, headers_end, body_len);
  if (c->up_len == 0) {
    c->static_reply = true;
    c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                  "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n"
                                  "Connection: close\r\n\r\nbad request\n");
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  int up = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (up < 0) {
    close_conn(c);
    return;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)g_backend_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(up, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
    close(up);
    close_conn(c);
    return;
  }
  c->up = up;
  add_fd(up, EPOLLOUT, c);
}

void read_client(app *a, conn *c) {
  if (c->in_len >= kRequestMax - 1) {
    close_conn(c);
    return;
  }
  ssize_t n = read(c->fd, c->in + c->in_len, kRequestMax - 1 - c->in_len);
  if (n == 0) {
    close_conn(c);
    return;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    close_conn(c);
    return;
  }
  c->in_len += (size_t)n;
  c->in[c->in_len] = 0;

  char *end = strstr(c->in, "\r\n\r\n");
  if (!end) {
    if (c->in_len >= kRequestMax - 1) close_conn(c);
    return;
  }

  size_t head_len = (size_t)(end - c->in) + 4;
  size_t body_len = 0;
  const char *cl = strcasestr(c->in, "Content-Length:");
  if (cl && cl < end) body_len = (size_t)strtoul(cl + 15, nullptr, 10);
  if (head_len + body_len > kRequestMax) {
    close_conn(c);
    return;
  }
  if (c->in_len < head_len + body_len) return;  // body still arriving

  char method[8] = {0}, path[1024] = {0};
  if (sscanf(c->in, "%7s %1023s", method, path) != 2) {
    close_conn(c);
    return;
  }

  g_requests++;

  if (g_standalone && strcmp(path, "/api/status") == 0) {
    static char body[8192];
    int len = app_status_json(a, body, sizeof(body));
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/events") == 0) {
    c->status_sse = true;
    c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                  "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                                  "Connection: keep-alive\r\n\r\n");
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/library") == 0) {
    char *body = (char *)malloc(65536);
    int len = body ? app_library_json(a, body, 65536) : 0;
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body ? body : "{}",
                                     body ? (size_t)len : 2);
    free(body);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/config") == 0) {
    if (strcmp(method, "POST") == 0) {
      static const char ok[] = "{\"ok\":true}";
      c->static_reply = true;
      c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", ok, sizeof(ok) - 1);
      mod_fd(c->fd, EPOLLOUT);
      return;
    }
    char body[1024];
    int len = app_config_json(body, sizeof(body));
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/schema") == 0) {
    char bindings[512];
    app_bindings_json(bindings, sizeof(bindings));
    char body[1024];
    int len = snprintf(body, sizeof(body),
                       "{\"note\":\"wire formats live in padwave_wire.hh\","
                       "\"bindings\":%s}",
                       bindings);
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/command") == 0) {
    if (strcmp(method, "POST") != 0) {
      c->static_reply = true;
      c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                    "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n"
                                    "Connection: close\r\n\r\n");
      mod_fd(c->fd, EPOLLOUT);
      return;
    }
    char result[512];
    int len = app_handle_command(a, c->in + head_len, body_len, result, sizeof(result));
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", result, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strncmp(path, "/api/", 5) == 0 && strcmp(path, "/api/front") != 0 && strcmp(path, "/api/front/events") != 0) {
    static const char body[] = "{\"ok\":false,\"error\":\"not implemented\"}";
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body,
                                     sizeof(body) - 1);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (strcmp(path, "/api/front/events") == 0) {
    c->front_sse = true;
    c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                  "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                                  "Connection: keep-alive\r\n\r\n");
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (strcmp(path, "/api/front") == 0) {
    char body[768];
    int len = front_json(body, sizeof(body));
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (strncmp(path, "/api/", 5) == 0) {
    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0 &&
        strcmp(method, "PUT") != 0 && strcmp(method, "DELETE") != 0) {
      close_conn(c);
      return;
    }
    start_proxy(c, end, body_len);
    return;
  }

  c->static_reply = true;
  if (strcmp(path, "/health") == 0) {
    static const char body[] = "{\"ok\":true}\n";
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body,
                                     sizeof(body) - 1);
  } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
    if (g_index_len <= sizeof(c->out)) {
      memcpy(c->out, g_index_response, g_index_len);
      c->out_len = g_index_len;
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
  mod_fd(c->fd, EPOLLOUT);
}

void flush_client(conn *c) {
  while (c->out_off < c->out_len) {
    ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        c->client_blocked = true;
        mod_fd(c->fd, EPOLLOUT);
        return;
      }
      close_conn(c);
      return;
    }
    c->out_off += (size_t)n;
  }
  c->out_off = c->out_len = 0;
  c->client_blocked = false;

  if (c->front_sse || c->status_sse) {
    mod_fd(c->fd, 0);
    return;
  }

  if (c->static_reply) {
    close_conn(c);
    return;
  }
  // Proxy: let the backend send more.
  if (c->up >= 0) mod_fd(c->up, EPOLLIN);
}

void write_upstream(conn *c) {
  while (c->up_off < c->up_len) {
    ssize_t n = write(c->up, c->up_req + c->up_off, c->up_len - c->up_off);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        mod_fd(c->up, EPOLLOUT);
        return;
      }
      close_conn(c);
      return;
    }
    c->up_off += (size_t)n;
  }
  c->up_sent = true;
  mod_fd(c->up, EPOLLIN);
}

void read_upstream(conn *c) {
  if (c->client_blocked || c->out_len - c->out_off >= kBufferMax - 4096) {
    mod_fd(c->up, 0);
    return;
  }
  size_t space = kBufferMax - c->out_len;
  ssize_t n = read(c->up, c->out + c->out_len, space);
  if (n == 0) {
    // Backend finished. Flush what is buffered, then close both.
    c->up_sent = true;
    int up = c->up;
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, up, nullptr);
    if (up < kMaxFds) g_owner[up] = nullptr;
    close(up);
    c->up = -1;
    flush_client(c);
    if (c->fd >= 0) {
      while (c->out_off < c->out_len) {
        ssize_t w = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (w <= 0) break;
        c->out_off += (size_t)w;
      }
      close_conn(c);
    }
    return;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    close_conn(c);
    return;
  }
  c->out_len += (size_t)n;
  flush_client(c);
}

int front_json(char *buf, size_t cap) {
  char node[64];
  int controller = find_dualsense(node, sizeof(node)) >= 0 ? 1 : 0;
  return snprintf(buf, cap,
                  "{\"ok\":true,\"front\":1,\"uptime_s\":%ld,\"requests\":%lu,"
                  "\"controller\":%d,"
                  "\"player\":{\"valid\":%d,\"pos\":%ld,\"dur\":%ld,\"sent\":%ld,"
                  "\"underruns\":%ld,\"paused\":%d},"
                  "\"input\":{\"valid\":%d,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,"
                  "\"l2\":%d,\"r2\":%d,\"buttons\":%u,\"battery\":%d,\"jack\":%d}}",
                  (long)((monotonic_ms() - g_start_ms) / 1000), g_requests, controller,
                  g_player.valid ? 1 : 0, g_player.pos, g_player.dur, g_player.sent,
                  g_player.underruns, g_player.paused, g_input.valid ? 1 : 0, g_input.lx, g_input.ly, g_input.rx,
                  g_input.ry, g_input.l2, g_input.r2, g_input.buttons, g_input.battery,
                  g_input.jack);
}

void append_status_event(app *a, conn *c) {
  if (c->out_off > 0) {
    if (c->out_off < c->out_len) memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
    c->out_len -= c->out_off;
    c->out_off = 0;
  }
  char body[8192];
  int n = app_status_json(a, body, sizeof(body));
  if (n <= 0) return;
  char *chunk = (char *)malloc((size_t)n + 64);
  if (!chunk) return;
  int m = snprintf(chunk, (size_t)n + 64, "%x\r\ndata: ", n + 6);
  memcpy(chunk + m, body, (size_t)n);
  m += n;
  memcpy(chunk + m, "\n\n\r\n", 4);
  m += 4;
  if (c->out_len + (size_t)m <= sizeof(c->out)) {
    memcpy(c->out + c->out_len, chunk, (size_t)m);
    c->out_len += (size_t)m;
    mod_fd(c->fd, EPOLLOUT);
  }
  free(chunk);
}

void append_front_event(conn *c) {
  if (c->out_off > 0) {
    if (c->out_off < c->out_len)
      memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
    c->out_len -= c->out_off;
    c->out_off = 0;
  }

  char body[768];
  int n = front_json(body, sizeof(body));
  if (n <= 0) return;
  char event[800];
  int e = snprintf(event, sizeof(event), "data: %s\n\n", body);
  if (e <= 0) return;
  char chunk[832];
  int m = snprintf(chunk, sizeof(chunk), "%x\r\n", e);
  if (m <= 0 || (size_t)m + (size_t)e + 2 > sizeof(chunk)) return;
  memcpy(chunk + m, event, (size_t)e);
  m += e;
  memcpy(chunk + m, "\r\n", 2);
  m += 2;

  if (c->out_len + (size_t)m > sizeof(c->out)) return;
  memcpy(c->out + c->out_len, chunk, (size_t)m);
  c->out_len += (size_t)m;
  mod_fd(c->fd, EPOLLOUT);
}

} // namespace

int server_main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8787;
  g_backend_port = argc > 2 ? atoi(argv[2]) : 8787;

  int n = snprintf(nullptr, 0,
                   "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                   "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",
                   kIndexHtmlSize);
  if ((size_t)n + kIndexHtmlSize >= sizeof(g_index_response)) {
    fprintf(stderr, "padwave serve: UI too large for the static buffer\n");
    return 1;
  }
  snprintf(g_index_response, sizeof(g_index_response),
           "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
           "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",
           kIndexHtmlSize);
  memcpy(g_index_response + n, kIndexHtml, kIndexHtmlSize);
  g_index_len = (size_t)n + kIndexHtmlSize;

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

  g_standalone = getenv("PADWAVE_STANDALONE") != nullptr;
  app *a = nullptr;
  if (g_standalone) a = app_create();

  g_start_ms = monotonic_ms();

  g_status_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (g_status_fd >= 0) {
    struct sockaddr_un status_addr;
    memset(&status_addr, 0, sizeof(status_addr));
    status_addr.sun_family = AF_UNIX;
    snprintf(status_addr.sun_path, sizeof(status_addr.sun_path), "%s", padwave_status::kPath);
    unlink(padwave_status::kPath);
    if (bind(g_status_fd, (struct sockaddr *)&status_addr, sizeof(status_addr)) != 0) {
      close(g_status_fd);
      g_status_fd = -1;
    }
  }

  g_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (g_timer_fd >= 0) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_nsec = 500000000;
    its.it_value = its.it_interval;
    timerfd_settime(g_timer_fd, 0, &its, nullptr);
  }

  g_epoll = epoll_create1(EPOLL_CLOEXEC);
  if (g_epoll < 0) {
    perror("epoll_create1");
    return 1;
  }
  add_fd(listen_fd, EPOLLIN, nullptr);
  if (signal_fd >= 0) add_fd(signal_fd, EPOLLIN, nullptr);
  if (g_status_fd >= 0) add_fd(g_status_fd, EPOLLIN, nullptr);
  if (g_timer_fd >= 0) add_fd(g_timer_fd, EPOLLIN, nullptr);
  if (g_standalone) {
    int fds[4];
    int n = app_fds(a, fds, 4);
    for (int i = 0; i < n; i++) add_fd(fds[i], EPOLLIN, nullptr);
  }

  fprintf(stderr, "padwave serve: http://0.0.0.0:%d (%zu byte UI)\n", port, kIndexHtmlSize);

  struct epoll_event events[64];
  bool running = true;
  while (running) {
    int ready = epoll_wait(g_epoll, events, 64, -1);
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
            if (g_conns[j].fd < 0) {
              slot = &g_conns[j];
              break;
            }
          if (!slot) {
            close(client);
            continue;
          }
          *slot = conn{};
          slot->fd = client;
          add_fd(client, EPOLLIN, slot);
        }
        continue;
      }
      if (fd == g_timer_fd) {
        uint64_t expirations = 0;
        if (read(g_timer_fd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
          if (g_standalone) {
            app_tick(a);
            int fds[4];
            int n = app_fds(a, fds, 4);
            for (int i = 0; i < n; i++) add_fd(fds[i], EPOLLIN, nullptr);
          }
          for (int j = 0; j < kMaxConns; j++) {
            if (g_conns[j].fd < 0) continue;
            if (g_conns[j].front_sse) append_front_event(&g_conns[j]);
            if (g_conns[j].status_sse) append_status_event(a, &g_conns[j]);
          }
        }
        continue;
      }
      if (fd == g_status_fd) {
        char line[256];
        ssize_t n = recv(g_status_fd, line, sizeof(line) - 1, 0);
        if (n > 0) {
          line[n] = 0;
          parse_status(line);
        }
        continue;
      }
      if (fd == signal_fd) {
        struct signalfd_siginfo info;
        if (read(signal_fd, &info, sizeof(info)) == sizeof(info)) running = false;
        continue;
      }

      if (g_standalone && app_owns_fd(a, fd)) {
        app_handle_fd(a, fd);
        continue;
      }

      conn *c = (fd >= 0 && fd < kMaxFds) ? g_owner[fd] : nullptr;
      if (!c) continue;

      if (fd == c->fd) {
        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          close_conn(c);
          continue;
        }
        if (events[i].events & EPOLLIN) read_client(a, c);
        if (c->fd >= 0 && (events[i].events & EPOLLOUT)) flush_client(c);
      } else if (fd == c->up) {
        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          close_conn(c);
          continue;
        }
        if (events[i].events & EPOLLOUT) write_upstream(c);
        if (c->up >= 0 && (events[i].events & EPOLLIN)) read_upstream(c);
      }
    }
  }

  for (int i = 0; i < kMaxConns; i++) close_conn(&g_conns[i]);
  if (g_status_fd >= 0) {
    close(g_status_fd);
    unlink(padwave_status::kPath);
  }
  if (g_timer_fd >= 0) close(g_timer_fd);
  if (a) app_destroy(a);
  close(g_epoll);
  if (signal_fd >= 0) close(signal_fd);
  close(listen_fd);
  return 0;
}// padwave — one binary, the native modes.
//
//   padwave native            the player: decode, Opus and 0x36/0x31
//   padwave input             the input decoder: 0x31 -> state lines
//   padwave serve [port]      the bootstrap web server (epoll, embedded UI)
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