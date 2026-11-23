// padwave_app.hh — the app and the engine interface it drives.
//
// The app is the one stateful object in the binary: the library, the current
// track, the playback counters, the controller snapshot, the persisted
// config and the lab output deadlines. Its fields are private; the server only
// sees the methods it needs. The implementation is inline because the project
// is one translation unit.
#pragma once

#include <dirent.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <pthread.h>

#ifdef __linux__
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#endif

#include "padwave_player.hh"
#include "padwave_input.hh"
#include "padwave_sink.hh"
#include "dualsense-extra.hh"

// The Bluetooth address from the hidraw uevent's HID_UNIQ. bluetoothctl and
// the kernel both spell it lower-case with colons.

// The app owns the library, config and text helpers.
// ---------------------------------------------------------------------------
// The app
// ---------------------------------------------------------------------------

struct App {
  friend struct Player;
  friend struct Input;
  friend struct Server;

private:
  static constexpr int kMaxTracks  = 4096;
  static constexpr int kMaxPath    = 1024;
  static constexpr int kMaxTitle   = 160;
  static constexpr int kMaxFolder  = 96;
  static constexpr int kMaxHistory = 48;
  static constexpr int kMaxLiveUrl = 2048;
  static constexpr int kMaxLiveChannels = 400;

  static constexpr size_t kLivePcmSeconds = 20;
  static constexpr size_t kPlaylistBytes  = 4 * 1024 * 1024;
  static constexpr size_t kSegmentBytes   = 32 * 1024 * 1024;

  struct CurlBuffer {
    char *data;
    size_t cap;
    size_t len;
    bool overflow;
  };

  struct Track {
    char path[kMaxPath];
    char title[kMaxTitle];
    char folder[kMaxFolder];
    char ext[8];
    char id[17];
    int64_t duration_ms = 0;
    int64_t size        = 0;
    int64_t mtime_sec   = 0;
    long mtime_nsec     = 0;
    bool favorite       = false;
  };

  struct LiveChannel {
    char name[kMaxTitle];
    char group[kMaxFolder];
    char logo[256];
    char number[16];
    char country[32];
    char provider[64];
    char language[32];
    char url[kMaxPath];
    bool radio;
  };

  static void debugLog(const char *format, ...) {
    FILE *f = fopen("/tmp/padwave-debug.log", "a");
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(f, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, format);
    vfprintf(f, format, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
  }

  static long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
  }

  static void makeId(const char *path, char *out) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(path); *p; p++) {
      h ^= *p;
      h *= 1099511628211ULL;
    }
    snprintf(out, 17, "%016llx", static_cast<unsigned long long>(h));
  }

  static bool audioExt(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *exts[] = {".flac", ".wav", ".mp3", ".ogg", ".opus", ".m4a", ".aac", ".wma"};
    for (const char *e : exts)
      if (strcasecmp(dot, e) == 0) return true;
    return false;
  }

  static unsigned char lowerAscii(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
  }

  static bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }

  // Compare digit runs by value, not by their spelling: ep2 < ep10, and
  // ep10 < ep10-1 < ep11 < ep100.
  static int naturalCompare(const char *a, const char *b) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(a);
    const unsigned char *q = reinterpret_cast<const unsigned char *>(b);
    while (*p && *q) {
      if (isDigit(*p) && isDigit(*q)) {
        const unsigned char *as = p;
        const unsigned char *bs = q;
        while (*as == '0') as++;
        while (*bs == '0') bs++;
        const unsigned char *ae = as;
        const unsigned char *be = bs;
        while (isDigit(*ae)) ae++;
        while (isDigit(*be)) be++;

        size_t alen = static_cast<size_t>(ae - as);
        size_t blen = static_cast<size_t>(be - bs);
        if (alen != blen) return alen < blen ? -1 : 1;
        int c = memcmp(as, bs, alen);
        if (c) return c < 0 ? -1 : 1;

        while (isDigit(*p)) p++;
        while (isDigit(*q)) q++;
        continue;
      }

      unsigned char ca = lowerAscii(*p);
      unsigned char cb = lowerAscii(*q);
      if (ca != cb) return ca < cb ? -1 : 1;
      p++;
      q++;
    }
    if (*p) return 1;
    if (*q) return -1;
    return 0;
  }

  static int compareTracks(const void *a, const void *b) {
    const Track *x = reinterpret_cast<const Track *>(a);
    const Track *y = reinterpret_cast<const Track *>(b);
    int c          = naturalCompare(x->folder, y->folder);
    return c ? c : naturalCompare(x->title, y->title);
  }

  static void kvAdd(char *buf, size_t cap, size_t *len, const char *key, const char *fmt, ...) {
    size_t start = *len;
    if (start + 2 > cap) return;
    int n = snprintf(buf + start, cap - start, "%s=", key);
    if (n <= 0 || static_cast<size_t>(n) >= cap - start) return;
    *len = start + static_cast<size_t>(n);

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (m < 0 || static_cast<size_t>(m) + 2 > cap - *len) {
      *len       = start;
      buf[start] = 0;
      return;
    }
    *len += static_cast<size_t>(m);

    buf[(*len)++] = '\n';
    buf[*len]     = 0;
  }

  static void kvText(char *buf, size_t cap, size_t *len, const char *key, const char *value) {
    size_t start = *len;
    if (start + 2 > cap) return;
    int n = snprintf(buf + start, cap - start, "%s=", key);
    if (n <= 0 || static_cast<size_t>(n) >= cap - start) return;
    *len = start + static_cast<size_t>(n);

    for (const char *p = value; p && *p; p++) {
      if (*len + 2 >= cap) break;
      buf[(*len)++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
    }
    buf[(*len)++] = '\n';
    buf[*len]     = 0;
  }

  static bool kvGet(const char *body, const char *key, char *out, size_t cap) {
    if (!body || cap == 0) return false;
    size_t klen = strlen(key);
    for (const char *p = body; *p;) {
      const char *eol = strchr(p, '\n');
      size_t n        = eol ? static_cast<size_t>((eol - p)) : strlen(p);
      if (n > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
        const char *v = p + klen + 1;
        size_t vn     = n - klen - 1;
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

  static long kvNumber(const char *body, const char *key, long fallback) {
    char tmp[32];
    if (!kvGet(body, key, tmp, sizeof(tmp))) return fallback;
    return atol(tmp);
  }

  static bool configPath(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    snprintf(buf, cap, "%s/.config/padwave/config", home);
    return true;
  }

  static bool favoritesPath(char *buf, size_t cap) {
    char config[512];
    if (!configPath(config, sizeof(config))) return false;
    char *slash = strrchr(config, '/');
    if (!slash) return false;
    snprintf(slash + 1, sizeof(config) - static_cast<size_t>(slash + 1 - config), "favorites.tsv");
    snprintf(buf, cap, "%s", config);
    return true;
  }

  static bool cachePath(const char *name, char *buf, size_t cap) {
    const char *xdg  = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
      snprintf(buf, cap, "%s/padwave/%s", xdg, name);
    else if (home && *home)
      snprintf(buf, cap, "%s/.cache/padwave/%s", home, name);
    else
      return false;
    return true;
  }

  static void ensureCacheDir(const char *path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = 0;
    for (char *p = dir + 1; *p; p++) {
      if (*p != '/') continue;
      *p = 0;
      mkdir(dir, 0755);
      *p = '/';
    }
    mkdir(dir, 0755);
  }

  static void fileField(FILE *f, const char *value) {
    for (const char *p = value; p && *p; p++)
      fputc((*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p, f);
  }

  static int64_t probeDuration(const char *path) {
    AVFormatContext *fmt = nullptr;
    AVDictionary *opts   = nullptr;
    av_dict_set(&opts, "probesize", "262144", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);
    int opened = avformat_open_input(&fmt, path, nullptr, &opts);
    av_dict_free(&opts);
    if (opened < 0) return 0;
    if (fmt->duration <= 0 || fmt->duration == AV_NOPTS_VALUE)
      avformat_find_stream_info(fmt, nullptr);
    int64_t duration = fmt->duration;
    avformat_close_input(&fmt);
    return duration > 0 && duration != AV_NOPTS_VALUE ? duration / 1000 : 0;
  }

  void loadDurationCache() {
    char path[512];
    if (!cachePath("durations.tsv", path, sizeof(path))) return;
    FILE *f = fopen(path, "r");
    if (f) {
      char line[2048];
      while (fgets(line, sizeof(line), f)) {
        char *fields[4];
        char *p = line;
        bool complete = true;
        for (int i = 0; i < 4; i++) {
          fields[i] = p;
          char *tab = strchr(p, '\t');
          if (!tab) {
            complete = false;
            break;
          }
          *tab = 0;
          p    = tab + 1;
        }
        if (!complete) continue;
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        int64_t duration = strtoll(fields[0], nullptr, 10);
        int64_t mtime_s   = strtoll(fields[1], nullptr, 10);
        long mtime_ns     = strtol(fields[2], nullptr, 10);
        int64_t size      = strtoll(fields[3], nullptr, 10);
        if (duration <= 0) continue;
        int index = findTrack(p);
        if (index < 0) continue;
        Track *t = &this->tracks[index];
        if (t->size == size && t->mtime_sec == mtime_s && t->mtime_nsec == mtime_ns)
          t->duration_ms = duration;
      }
      fclose(f);
    }
    this->duration_pending = 0;
    for (int i = 0; i < this->count; i++)
      if (this->tracks[i].duration_ms <= 0) this->duration_pending++;
  }

  void loadFavorites() {
    char path[512];
    if (!favoritesPath(path, sizeof(path))) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[kMaxPath + 2];
    while (fgets(line, sizeof(line), f)) {
      char *nl = strchr(line, '\n');
      if (nl) *nl = 0;
      nl = strchr(line, '\r');
      if (nl) *nl = 0;
      if (!line[0]) continue;
      int index = findTrack(line);
      if (index >= 0) this->tracks[index].favorite = true;
    }
    fclose(f);
  }

  void saveFavoritesLocked() {
    char path[512];
    if (!favoritesPath(path, sizeof(path))) return;
    ensureCacheDir(path);
    char tmp[544];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < this->count; i++) {
      if (!this->tracks[i].favorite) continue;
      fileField(f, this->tracks[i].path);
      fputc('\n', f);
    }
    if (fclose(f) != 0) return;
    rename(tmp, path);
  }

  bool toggleFavorite(const char *id, bool *value) {
    int index = findId(id);
    if (index < 0) return false;
    pthread_mutex_lock(&library_mutex);
    this->tracks[index].favorite = !this->tracks[index].favorite;
    *value                       = this->tracks[index].favorite;
    saveFavoritesLocked();
    writeLibraryFileLocked();
    pthread_mutex_unlock(&library_mutex);
    return true;
  }

  void writeLibraryFileLocked() {
    char path[512];
    if (!cachePath("library.tsv", path, sizeof(path))) return;
    ensureCacheDir(path);
    char tmp[544];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < this->count; i++) {
      const Track *s = &this->tracks[i];
      fileField(f, s->id);
      fputc('\t', f);
      fileField(f, s->title);
      fputc('\t', f);
      fileField(f, s->folder);
      fputc('\t', f);
      fileField(f, s->path);
      fputc('\t', f);
      fileField(f, s->ext);
      fprintf(f, "\t%lld\t%d\n", static_cast<long long>(s->duration_ms), s->favorite ? 1 : 0);
    }
    if (fclose(f) != 0) return;
    rename(tmp, path);
  }

  void writeLibraryFile() {
    pthread_mutex_lock(&library_mutex);
    writeLibraryFileLocked();
    pthread_mutex_unlock(&library_mutex);
  }

  void scanDurations() {
    char cache[512];
    FILE *cache_file = nullptr;
    if (cachePath("durations.tsv", cache, sizeof(cache))) {
      ensureCacheDir(cache);
      cache_file = fopen(cache, "a");
    }

    for (int i = 0; i < this->count; i++) {
      char path[kMaxPath];
      int64_t size = 0, mtime_s = 0;
      long mtime_ns = 0;
      bool need = false;
      pthread_mutex_lock(&library_mutex);
      if (this->tracks[i].duration_ms <= 0) {
        snprintf(path, sizeof(path), "%s", this->tracks[i].path);
        size       = this->tracks[i].size;
        mtime_s    = this->tracks[i].mtime_sec;
        mtime_ns   = this->tracks[i].mtime_nsec;
        need       = true;
      }
      pthread_mutex_unlock(&library_mutex);
      if (!need) continue;

      int64_t duration = probeDuration(path);
      pthread_mutex_lock(&library_mutex);
      if (duration > 0 && this->tracks[i].duration_ms <= 0) {
        this->tracks[i].duration_ms = duration;
        if (cache_file) {
          fprintf(cache_file, "%lld\t%lld\t%ld\t%lld\t",
                  static_cast<long long>(duration), static_cast<long long>(mtime_s), mtime_ns,
                  static_cast<long long>(size));
          fileField(cache_file, path);
          fputc('\n', cache_file);
          fflush(cache_file);
        }
      }
      if (this->duration_pending > 0) this->duration_pending--;
      pthread_mutex_unlock(&library_mutex);
    }
    if (cache_file) fclose(cache_file);

    pthread_mutex_lock(&library_mutex);
    writeLibraryFileLocked();
    this->library_generation++;
    this->duration_scanning = false;
    pthread_mutex_unlock(&library_mutex);
  }

  static void *durationWorkerEntry(void *arg) {
    static_cast<App *>(arg)->scanDurations();
    return nullptr;
  }

  void startDurationScan() {
    pthread_mutex_lock(&library_mutex);
    bool start = !this->duration_scanning && this->duration_pending > 0;
    if (start) this->duration_scanning = true;
    pthread_mutex_unlock(&library_mutex);
    if (!start) return;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, &App::durationWorkerEntry, this) == 0) {
      pthread_detach(thread);
    } else {
      pthread_mutex_lock(&library_mutex);
      this->duration_scanning = false;
      pthread_mutex_unlock(&library_mutex);
    }
  }

  static void tsvField(char *buf, size_t cap, size_t *o, const char *value, char sep) {
    for (const char *p = value; p && *p && *o + 2 < cap; p++)
      buf[(*o)++] = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
    if (*o + 1 < cap) buf[(*o)++] = sep;
    buf[*o] = 0;
  }

  Track tracks[kMaxTracks];
  int count   = 0;
  int current = -1;
  mutable pthread_mutex_t library_mutex = PTHREAD_MUTEX_INITIALIZER;
  int duration_pending                  = 0;
  int library_generation                = 0;
  bool duration_scanning                = false;
  static inline LiveChannel live_channels[kMaxLiveChannels];
  int live_count             = 0;
  char playback_mode[8]             = "idle";
  char stream_url[kMaxLiveUrl]      = "";
  bool stream_resume_pending        = false;
  long stream_resume_at             = 0;
  char live_url[kMaxLiveUrl]        = "";
  char live_direct_url[kMaxLiveUrl] = "";
  char live_source_url[kMaxLiveUrl] = "";
  char live_upstream_url[kMaxPath]  = "";
  char live_decoded[kMaxPath]       = "";
  char live_proxy[512]              = "";
  char live_proxy_user[128]         = "";
  char live_proxy_pass[128]         = "";
  int http_port                     = 8787;
  bool live_insecure                = false;
  bool live_has_video               = false;
  char live_error[160]              = "";
  int live_current           = -1;
  bool live_active           = false;
  bool live_audio_synced     = false;
  long live_audio_seq        = 0;
  long live_audio_at         = 0;
  CURLM *live_multi          = nullptr;
  CURL *live_pl_curl         = nullptr;
  CURL *live_seg_curl        = nullptr;
  bool live_pl_inflight      = false;
  bool live_seg_inflight     = false;
  CurlBuffer live_pl_buf{};
  CurlBuffer live_seg_buf{};
  bool live_next_ready       = false;
  long live_next_seq         = 0;
  double live_next_dur       = 0;
  char live_next_url[kMaxPath * 2] = "";
  static inline float live_pcm[kLivePcmSeconds * RATE * CHANNELS];
  size_t live_pcm_frames = 0;
  size_t live_pcm_pos    = 0;
  long live_decode_ok    = 0;
  long live_decode_fail  = 0;
  static inline uint8_t web_pcm[512 * 1024];
  uint64_t web_pcm_head     = 0;
  int web_audio_clients     = 0;
  bool live_raw          = false;
  CURL *live_raw_curl    = nullptr;
  bool live_raw_inflight = false;
  bool live_pl_proxied   = false;
  bool live_seg_proxied  = false;
  bool live_raw_proxied  = false;
  struct curl_slist *live_raw_headers = nullptr;
  static inline uint8_t live_raw_ring[4 * 1024 * 1024];
  size_t live_raw_head   = 0;
  size_t live_raw_tail   = 0;
  int live_raw_codec     = -1;
  AVCodecParserContext *live_raw_parser = nullptr;
  AVCodecContext *live_raw_dec          = nullptr;
  SwrContext *live_raw_swr              = nullptr;
  AVFrame *live_raw_frame               = nullptr;
  int live_ogg_serial                   = -1;
  size_t live_ogg_packet_len            = 0;
  uint8_t live_ogg_packet[131072];
  int history[kMaxHistory];
  int history_len = 0;

  static inline Player player;
  Input input;
  int player_timer_fd   = -1;
  bool sink_node_active = false;
  bool sink_playback    = false;
  PipeWireSink sink;

  static constexpr int kDefaultVolume = 15;

  long pos_ms             = 0;
  long dur_ms             = 0;
  long sent               = 0;
  long underruns          = 0;
  bool eof                = false;
  bool playing            = false;
  bool paused             = false;
  bool paused_by_volume   = false;
  int volume              = kDefaultVolume;
  int last_nonzero_volume = kDefaultVolume;
  int speed               = 100;

  bool controller          = false;
  bool jack                = false;
  bool plugged_usb_data    = false;
  bool pluggedUsbPower     = false;
  // Runtime-only fallback for a shared USB host. Never persisted; cleared
  // whenever the USB data link goes away.
  bool controller_disabled = false;
  int battery              = -1;
  int rssi                 = 0;
  bool rssi_valid          = false;
  int link_quality         = -1;
  int tx_power             = -1;
  int afh_channels         = -1;
  int afh_mode             = -1;
  int supervision_ms       = -1;
  int link_policy          = -1;
  char afh_map[24]         = "";
  long rssi_at             = 0;
  char charging[16]        = "unknown";
  char powerState[32]      = "unknown";
  int lx = 128, ly = 128, rx = 128, ry = 128;
  unsigned l2 = 0, r2 = 0;
  char dpad[16]     = "none";
  char buttons[256] = "";
  int temp          = 0;
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  int rate                  = 0;
  unsigned host_timestamp   = 0;
  unsigned device_timestamp = 0;
  unsigned trigger_effect_r = 0;
  unsigned trigger_effect_l = 0;
  unsigned trigger_stop_r   = 0;
  unsigned trigger_stop_l   = 0;
  unsigned trigger_status_r = 0;
  unsigned trigger_status_l = 0;
  unsigned touch_active[2]  = {};
  unsigned touch_id[2]      = {};
  unsigned touch_x[2]       = {};
  unsigned touch_y[2]       = {};
  long rate_at              = 0;
  unsigned rate_frames      = 0;
  char input_node[64]       = "";
  char controller_mac[32]   = "";
  char saved_controller_mac[32] = "";
  char last_action[64]      = "";
  char raw[400]             = "";

  // Where the last session left off.
  char resume_path[kMaxPath] = "";
  long resume_pos_ms         = 0;
  bool resume_pending        = false;
  bool playhead_dirty        = false;
  long playhead_saved_at     = 0;
  bool volume_dirty          = false;
  long volume_saved_at       = 0;

  int reconnect_at                        = 0;
  static constexpr int kPowerSaveModeMask = 0x17;
  long idle_since                         = 0;
  long last_activity_ms                   = 0;
  bool idle_warning                       = false;
  bool suspended                          = false;
  bool paused_by_idle                     = false;
  bool paused_by_jack                     = false;
  bool paused_by_disconnect               = false;
  bool paused_by_mic                      = false;
  bool idle_volume_saved                  = false;
  int idle_volume                         = 0;
  int idle_fade_volume                    = -1;
  long idle_activity_ignore_until         = 0;
  long poweroff_at                        = 0;
  bool powered_off                        = false;

  // Config that the panel edits; persisted as ~/.config/padwave/config.
  int idle_timeout_ms       = 900000;
  int idle_warn_grace_ms    = 60000;
  int poweroff_when_idle_ms = 60000;
  bool haptic_subwoofer     = false;
  bool haptics_only         = false;
  int haptic_cutoff_hz      = 150;
  int haptic_gain           = 100;

  int rumble_left = 0, rumble_right = 0;
  long rumble_until      = 0;
  long idle_rumble_until = 0;

  // The state-coloured lightbar and the player-LED ownership. The player's
  // 0x36 snapshot carries whatever `fields` last merged into its OutputBody.
  int led_custom[3]                 = {0, 0, 0};
  bool led_custom_set               = false;
  bool led_force                    = true;
  bool led_released                 = false;
  int led_last_rgb[3]               = {-1, -1, -1};
  int led_last_player               = -2;
  int led_last_state                = -1;
  bool led_warning_high             = true;
  long led_warning_at               = 0;
  long led_battery_at               = 0;
  int led_battery_phase             = 0;
  static constexpr long kLedBlinkMs = 2000;
  bool led_manual_player            = false;
  int led_manual_mask               = 4;
  bool led_painted                  = false;
  bool lightbar_on                  = true;
  int power_save_mask               = 0;
  bool power_save_mode              = false;
  bool power_save_leds_applied      = false;
  int power_save_saved_mask         = 0;
  bool lightbar_saved_on            = true;
  long led_charge_at                = 0;
  dualsense::OutputBody::LightbarSetup led_charge_setup =
      dualsense::OutputBody::LightbarSetup::LightOut;
  static constexpr long kChargeBlinkMs = 1000;

  unsigned long mic_packets = 0;
  unsigned long mic_frames  = 0;
  unsigned long mic_samples = 0;
  int mic_rms               = 0;
  int mic_peak              = 0;
  int mic_level             = 0;
  long mic_last_ms          = 0;
  bool mic_monitor          = false;

public:
  bool init() {
    signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    this->live_multi = curl_multi_init();
    this->live_pl_buf = {static_cast<char *>(malloc(kPlaylistBytes)), kPlaylistBytes, 0, false};
    this->live_seg_buf = {static_cast<char *>(malloc(kSegmentBytes)), kSegmentBytes, 0, false};

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
        scanDir(expanded, "");
      } else {
        scanDir(dir, "");
      }
    }

    qsort(this->tracks, static_cast<size_t>(this->count), sizeof(Track), compareTracks);
    loadDurationCache();
    loadFavorites();
    writeLibraryFile();
    startDurationScan();
    srand(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid()));
    this->player_timer_fd = -1;
    configLoad();
    loadLiveCache();
    if (strcmp(this->playback_mode, "stream") == 0 && validLiveUrl(this->stream_url)) {
      this->stream_resume_pending = true;
    } else if (this->resume_path[0]) {
      int idx = findTrack(this->resume_path);
      if (idx >= 0) {
        this->current        = idx;
        this->pos_ms         = this->resume_pos_ms;
        this->resume_pending = true;
      }
    }
    this->playhead_saved_at = nowMs();
    this->last_activity_ms  = this->playhead_saved_at;
    // configLoad() restored the current and last non-zero volumes. A current
    // non-zero value is by definition the latest one; only fall back to 15
    // when no non-zero value has ever been stored.
    if (this->volume > 0)
      this->last_nonzero_volume = this->volume;
    else if (this->last_nonzero_volume <= 0)
      this->last_nonzero_volume = kDefaultVolume;
    this->volume_dirty     = false;
    this->sink_node_active = this->sink.start(&App::sinkPush, &this->player);
    if (!this->sink_node_active) fprintf(stderr, "padwave: PipeWire sink unavailable\n");
    return true;
  }

  bool sinkAvailable() const { return this->sink_node_active; }

  void setHttpPort(int port) { this->http_port = port; }

  static void sinkPush(void *owner, const float *samples, size_t frames) {
    static_cast<Player *>(owner)->pushSink(samples, frames);
  }

  bool sinkPlaybackStart() {
    if (!this->sink_node_active || this->sink_playback) return this->sink_playback;
    if (!this->player) playerStart();
    if (!this->player) return false;
    this->player.setSinkMode(true);
    this->sink_playback = true;
    this->playing       = false;
    this->paused        = true;
    return true;
  }

  void sinkPlaybackStop() {
    if (!this->sink_playback) return;
    this->sink_playback = false;
    this->player.setSinkMode(false);
  }

  void shutdown() {
    char saved_mode[8];
    char saved_stream[kMaxLiveUrl];
    snprintf(saved_mode, sizeof(saved_mode), "%s", this->playback_mode);
    snprintf(saved_stream, sizeof(saved_stream), "%s", this->stream_url);
    stopLive(true);
    sinkPlaybackStop();
    this->sink.stop();
    this->sink_node_active = false;
    this->playing          = false;
    this->controller       = false;
    this->led_force        = true;
    serviceLeds();
    configSave();
    if (this->player) {
      playerSend("quit\n");
      this->player.destroy();
    }
    this->player_timer_fd = -1;
    inputStop(false);
    snprintf(this->playback_mode, sizeof(this->playback_mode), "%s", saved_mode);
    snprintf(this->stream_url, sizeof(this->stream_url), "%s", saved_stream);
    configSave();
    micReset();
    this->web_pcm_head = 0;
    this->web_audio_clients = 0;
    if (this->live_multi) {
      curl_multi_cleanup(this->live_multi);
      this->live_multi = nullptr;
    }
    free(this->live_pl_buf.data);
    free(this->live_seg_buf.data);
    this->live_pl_buf.data  = nullptr;
    this->live_seg_buf.data = nullptr;
    curl_global_cleanup();
  }

  void resetLinkStats() {
    this->rssi_valid     = false;
    this->link_quality   = -1;
    this->tx_power       = -1;
    this->afh_channels   = -1;
    this->afh_mode       = -1;
    this->supervision_ms = -1;
    this->link_policy    = -1;
    this->afh_map[0]     = 0;
  }

  void updateLinkStats() {
    resetLinkStats();
    if (!this->controller || this->player.isUsb() || !this->controller_mac[0]) return;
#ifdef __linux__
    bdaddr_t addr;
    if (str2ba(this->controller_mac, &addr) < 0) return;
    int dev_id = hci_get_route(&addr);
    if (dev_id < 0) return;
    int dd = hci_open_dev(dev_id);
    if (dd < 0) return;

    alignas(hci_conn_info) unsigned char
        conn_buf[sizeof(hci_conn_info_req) + sizeof(hci_conn_info)] = {};
    auto *req  = reinterpret_cast<hci_conn_info_req *>(conn_buf);
    auto *info = reinterpret_cast<hci_conn_info *>(conn_buf + sizeof(hci_conn_info_req));
    bacpy(&req->bdaddr, &addr);
    req->type = ACL_LINK;
    if (ioctl(dd, HCIGETCONNINFO, req) == 0) {
      uint16_t handle = info->handle;
      int8_t rssiValue;
      uint8_t lqValue;
      int8_t txValue;
      uint16_t timeout = 0;
      uint16_t policy  = 0;
      uint8_t mode     = 0;
      uint8_t map[10]  = {};
      if (hci_read_rssi(dd, handle, &rssiValue, 1000) == 0) {
        this->rssi       = rssiValue;
        this->rssi_valid = true;
      }
      if (hci_read_link_quality(dd, handle, &lqValue, 1000) == 0) this->link_quality = lqValue;
      if (hci_read_transmit_power_level(dd, handle, 0, &txValue, 1000) == 0)
        this->tx_power = txValue;
      if (hci_read_link_supervision_timeout(dd, handle, &timeout, 1000) == 0 && timeout != 0xFFFF)
        this->supervision_ms = timeout * 625 / 1000;
      if (hci_read_link_policy(dd, handle, &policy, 1000) == 0) this->link_policy = policy;
      if (hci_read_afh_map(dd, handle, &mode, map, 1000) == 0) {
        this->afh_mode = mode;
        int channels   = 0;
        for (int i = 0; i < 10; ++i) {
          int byte = map[i] & (i == 9 ? 0x7F : 0xFF);
          channels += __builtin_popcount(static_cast<unsigned>(byte));
        }
        this->afh_channels = channels;
        for (int i = 0; i < 10; ++i) snprintf(this->afh_map + i * 2, 3, "%02x", map[9 - i]);
      }
    }
    close(dd);
#endif
  }

  void tick() {
    serviceLiveFeed();
    serviceLiveMulti();
    long now = nowMs();
    if (this->stream_resume_pending && !this->live_active && now >= this->stream_resume_at) {
      if (!this->player) playerStart();
      if (this->player && this->player.fd() >= 0) {
        int idx = findLiveUrl(this->stream_url);
        bool ok = idx >= 0 ? startLive(idx) : startLiveDirect(this->stream_url);
        if (ok) {
          this->stream_resume_pending = false;
        } else {
          this->stream_resume_at = now + 2000;
        }
      }
    }
    // Invariant: playback running means the mic monitor is off, regardless of
    // which resume path got us here.
    serviceOutput();
    updateIdle();
    serviceLeds();
    if (this->eof) {
      this->eof = false;
      next();
    }
    if (!this->player) playerStart();
    if (!this->sink_playback && this->player.sinkInputSeen() && this->sinkPlaybackStart())
      this->player.clearSinkInput();
    if (!this->sink_playback && this->player) {
      if (this->resume_pending && this->current >= 0) {
        this->resume_pending = false;
        play(this->current, this->pos_ms);
      } else if (this->current < 0 && this->count > 0) {
        int start = rand() % this->count;
        play(start, 0);
      }
    }
    if (this->player && this->input.fd() >= 0 && this->input.fd() != this->player.fd()) {
      this->input.close();
      if (this->player.fd() >= 0) this->inputStart();
    }
    if (this->input.fd() < 0 && static_cast<int>(nowMs()) >= this->reconnect_at) inputStart();

    if (this->playhead_dirty && now - this->playhead_saved_at >= 1000) {
      this->playhead_dirty    = false;
      this->playhead_saved_at = now;
      configSave();
    } else if (this->current >= 0 && now - this->playhead_saved_at >= 30000) {
      this->playhead_saved_at = now;
      configSave();
    }
    if (this->volume_dirty && now - this->volume_saved_at >= 1000) {
      this->volume_dirty    = false;
      this->volume_saved_at = now;
      configSave();
    }
    if (now - this->rssi_at >= 2000) {
      this->rssi_at = now;
      updateLinkStats();
    }
  }

  int fds(int *fds, int max) {
    int n = 0;
    if (this->player_timer_fd >= 0 && n < max) fds[n++] = this->player_timer_fd;
    if (this->input.fd() >= 0 && n < max) fds[n++] = this->input.fd();
    if (this->sink.fd() >= 0 && n < max) fds[n++] = this->sink.fd();
    return n;
  }

  void handleFd(int fd) {
    if (fd == this->sink.fd()) {
      this->sink.dispatch();
      if (!this->sink_playback && this->player.sinkInputSeen() && this->sinkPlaybackStart())
        this->player.clearSinkInput();
      return;
    }
    if (this->player && fd == this->player_timer_fd) {
      this->serviceLiveFeed();
      this->player.step();
      serviceLiveMulti();
      serviceOutput();
      serviceLeds();
      return;
    }
    if (this->input.fd() >= 0 && fd == this->input.fd()) {
      uint8_t buf[512];
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n > 0) {
        this->input.feed(buf, static_cast<size_t>(n));
      } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
        inputStop(true);
        this->reconnect_at = static_cast<int>((nowMs() + 1500));
      }
    }
  }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdate-time"
  static constexpr const char *buildVersion = __DATE__ " " __TIME__;
#pragma clang diagnostic pop

  int statusText(char *buf, size_t cap) {
    size_t o = 0;
    if (cap) buf[0] = 0;
    kvText(buf, cap, &o, "server.version", buildVersion);
    pthread_mutex_lock(&library_mutex);
    int lib_count      = this->count;
    int lib_pending    = this->duration_pending;
    int lib_generation = this->library_generation;
    bool lib_scanning  = this->duration_scanning;
    pthread_mutex_unlock(&library_mutex);
    kvAdd(buf, cap, &o, "library.count", "%d", lib_count);
    kvAdd(buf, cap, &o, "library.pending_durations", "%d", lib_pending);
    kvAdd(buf, cap, &o, "library.generation", "%d", lib_generation);
    kvAdd(buf, cap, &o, "library.scanning", "%s", lib_scanning ? "true" : "false");
    const Track *s = this->current >= 0 ? &this->tracks[this->current] : nullptr;

    long now             = nowMs();
    long idle_ms         = now > this->last_activity_ms ? now - this->last_activity_ms : 0;
    bool active_playback = this->playing && !this->paused;
    long timeout         = this->idle_timeout_ms;
    long grace           = this->idle_warn_grace_ms;
    const char *phase    = "active";
    long warning_in      = (timeout + 999) / 1000;
    long shutdown_in     = 0;
    int fade_percent     = 100;
    bool idle_enabled    = this->controller;

    if (!idle_enabled) {
      warning_in   = 0;
      shutdown_in  = 0;
      fade_percent = 100;
    } else if (this->suspended) {
      phase        = "suspended";
      warning_in   = 0;
      fade_percent = 10;
    } else if (!active_playback) {
      long left  = timeout - idle_ms;
      warning_in = left > 0 ? (left + 999) / 1000 : 0;
    } else if (this->idle_warning && idle_ms >= timeout) {
      phase        = "warning";
      long over    = idle_ms - timeout;
      int progress = grace > 0 ? static_cast<int>((over * 100 / grace)) : 100;
      if (progress < 0) progress = 0;
      if (progress > 100) progress = 100;
      fade_percent = 100 - progress * 90 / 100;
      warning_in   = 0;
      long left    = timeout + grace - idle_ms;
      shutdown_in  = left > 0 ? (left + 999) / 1000 : 0;
    } else {
      long left  = timeout - idle_ms;
      warning_in = left > 0 ? (left + 999) / 1000 : 0;
    }

    kvAdd(buf, cap, &o, "radio.playing", "%s", this->playing ? "true" : "false");
    kvAdd(buf, cap, &o, "radio.paused", "%s", this->paused ? "true" : "false");
    kvAdd(buf, cap, &o, "radio.position", "%.1f", static_cast<double>(this->pos_ms) / 1000.0);
    kvAdd(buf, cap, &o, "radio.duration", "%.1f", static_cast<double>(this->dur_ms) / 1000.0);
    kvAdd(buf, cap, &o, "radio.volume", "%d", this->volume);
    kvAdd(buf, cap, &o, "radio.speed", "%d", this->speed);
    kvAdd(buf, cap, &o, "radio.muted", "%s", this->volume == 0 ? "true" : "false");
    kvAdd(buf, cap, &o, "radio.fade_percent", "%d", fade_percent);
    kvAdd(buf, cap, &o, "radio.paused_by_jack", "%s", this->paused_by_jack ? "true" : "false");
    kvAdd(buf, cap, &o, "radio.paused_by_disconnect", "%s",
          this->paused_by_disconnect ? "true" : "false");
    kvAdd(buf, cap, &o, "radio.paused_by_mic", "%s", this->paused_by_mic ? "true" : "false");
    if (this->idle_warning || this->suspended)
      kvAdd(buf, cap, &o, "radio.applied_volume", "%d",
            this->idle_fade_volume >= 0 ? this->idle_fade_volume : 0);
    else
      kvAdd(buf, cap, &o, "radio.applied_volume", "%d", this->volume);
    if (s) {
      kvText(buf, cap, &o, "radio.current.id", s->id);
      kvText(buf, cap, &o, "radio.current.title", s->title);
      kvText(buf, cap, &o, "radio.current.folder", s->folder);
      kvText(buf, cap, &o, "radio.current.path", s->path);
      kvText(buf, cap, &o, "radio.current.ext", s->ext);
      kvAdd(buf, cap, &o, "radio.current.favorite", "%s", s->favorite ? "true" : "false");
    }

    kvText(buf, cap, &o, "session.phase", phase);
    kvAdd(buf, cap, &o, "session.idle_enabled", "%s", idle_enabled ? "true" : "false");
    kvAdd(buf, cap, &o, "session.idle_seconds", "%ld", idle_ms / 1000);
    kvAdd(buf, cap, &o, "session.warning_in", "%ld", warning_in);
    kvAdd(buf, cap, &o, "session.shutdown_in", "%ld", shutdown_in);
    kvAdd(buf, cap, &o, "session.suspended", "%s", this->suspended ? "true" : "false");
    kvText(buf, cap, &o, "session.suspended_by",
           this->paused_by_idle ? "idle" : (this->paused_by_jack ? "jack" : "null"));
    kvAdd(buf, cap, &o, "session.powered_off", "%s", this->powered_off ? "true" : "false");
    long poweroff_in = 0;
    if (this->suspended && this->poweroff_at > now)
      poweroff_in = (this->poweroff_at - now + 999) / 1000;
    kvAdd(buf, cap, &o, "session.poweroff_in", "%ld", poweroff_in);

    kvAdd(buf, cap, &o, "audio.present", "%s", this->player.isValid() ? "true" : "false");
    kvText(buf, cap, &o, "audio.sink", this->player.audioSink());
    kvText(buf, cap, &o, "audio.route", audioPathName(this->player.getAudioPath()));
    kvText(buf, cap, &o, "audio.state",
           this->player.audioActive() ? "running" : (this->player.isValid() ? "ready" : "null"));
    kvAdd(buf, cap, &o, "audio.sink_available", "%s", sinkAvailable() ? "true" : "false");
    kvAdd(buf, cap, &o, "audio.system_sink", "%s", this->sink_playback ? "true" : "false");
    kvAdd(buf, cap, &o, "audio.browser_clients", "%d", this->web_audio_clients);
    kvAdd(buf, cap, &o, "audio.player_playing", "%s", this->player.isPlaying() ? "true" : "false");
    kvAdd(buf, cap, &o, "audio.player_fd", "%d", this->player.fd());
    kvText(buf, cap, &o, "live.url", this->live_url);
    kvText(buf, cap, &o, "live.direct_url", this->live_direct_url);
    kvText(buf, cap, &o, "live.proxy", this->live_proxy);
    kvText(buf, cap, &o, "live.proxy_user", this->live_proxy_user);
    kvAdd(buf, cap, &o, "live.insecure", "%s", this->live_insecure ? "true" : "false");
    kvAdd(buf, cap, &o, "live.has_video", "%s", this->live_has_video ? "true" : "false");
    kvAdd(buf, cap, &o, "live.pcm_frames", "%zu", this->live_pcm_frames);
    kvAdd(buf, cap, &o, "live.decode_ok", "%ld", this->live_decode_ok);
    kvAdd(buf, cap, &o, "live.decode_fail", "%ld", this->live_decode_fail);
    kvAdd(buf, cap, &o, "live.sink_mode", "%s", this->player.sinkMode() ? "true" : "false");
    kvAdd(buf, cap, &o, "live.raw", "%s", this->live_raw ? "true" : "false");
    kvAdd(buf, cap, &o, "live.raw_codec", "%d", this->live_raw_codec);
    kvAdd(buf, cap, &o, "live.ogg_serial", "%d", this->live_ogg_serial);
    kvAdd(buf, cap, &o, "live.ogg_packet", "%zu", this->live_ogg_packet_len);
    kvAdd(buf, cap, &o, "live.raw_bytes", "%zu", this->live_raw_head - this->live_raw_tail);
    kvAdd(buf, cap, &o, "live.count", "%d", this->live_count);
    kvAdd(buf, cap, &o, "live.playing", "%s", this->live_active ? "true" : "false");
    kvAdd(buf, cap, &o, "live.video", "%s", this->live_active ? "true" : "false");
    kvAdd(buf, cap, &o, "live.current", "%d", this->live_current >= 0 ? this->live_current + 1 : 0);
    kvText(buf, cap, &o, "live.error", this->live_error);
    kvText(buf, cap, &o, "playback.mode", this->playback_mode);
    kvText(buf, cap, &o, "playback.stream_url", this->stream_url);

    kvAdd(buf, cap, &o, "hid.power_save", "%d", this->power_save_mask);
    kvAdd(buf, cap, &o, "hid.power_save_mode", "%s", this->power_save_mode ? "true" : "false");
    kvAdd(buf, cap, &o, "hid.player_leds", "%d",
          this->led_last_player >= 0 ? this->led_last_player : 4);
    int state        = ledState();
    const char *mode = state == 0 ? "off" : (this->led_custom_set ? "custom" : ledStateName(state));
    if (this->controller && strcmp(this->charging, "charging") == 0)
      mode = "charging";
    else if (this->controller && strcmp(this->charging, "full") == 0)
      mode = "full";
    kvText(buf, cap, &o, "led.mode", mode);
    kvAdd(buf, cap, &o, "led.red", "%d", this->led_last_rgb[0] >= 0 ? this->led_last_rgb[0] : 0);
    kvAdd(buf, cap, &o, "led.green", "%d", this->led_last_rgb[1] >= 0 ? this->led_last_rgb[1] : 0);
    kvAdd(buf, cap, &o, "led.blue", "%d", this->led_last_rgb[2] >= 0 ? this->led_last_rgb[2] : 0);
    kvAdd(buf, cap, &o, "led.custom", "%s", this->led_custom_set ? "true" : "false");
    kvAdd(buf, cap, &o, "led.painted", "%s", this->led_painted ? "true" : "false");

    bool mic_active = this->mic_last_ms > 0 && now - this->mic_last_ms < 500;
    kvAdd(buf, cap, &o, "mic.level", "%d", this->mic_level);
    kvAdd(buf, cap, &o, "mic.active", "%s", mic_active ? "true" : "false");
    kvAdd(buf, cap, &o, "mic.frames", "%lu", this->mic_frames);
    kvAdd(buf, cap, &o, "mic.packets", "%lu", this->mic_packets);
    kvAdd(buf, cap, &o, "mic.samples", "%lu", this->mic_samples);
    kvAdd(buf, cap, &o, "mic.rms", "%d", this->mic_rms);
    kvAdd(buf, cap, &o, "mic.peak", "%d", this->mic_peak);
    kvAdd(buf, cap, &o, "mic.running", "%s",
          this->mic_monitor && this->input.micReady() ? "true" : "false");
    kvAdd(buf, cap, &o, "mic.monitor", "%s", this->mic_monitor ? "true" : "false");

    kvAdd(buf, cap, &o, "controller.connected", "%s", this->controller ? "true" : "false");
    kvText(buf, cap, &o, "controller.bus", this->player.isUsb() ? "usb" : "bluetooth");
    kvText(buf, cap, &o, "controller.name", "DualSense");
    kvText(buf, cap, &o, "controller.mac", this->player.isUsb() ? "" : this->controller_mac);
    kvText(buf, cap, &o, "controller.saved_mac", this->saved_controller_mac);
    kvText(buf, cap, &o, "controller.model", "dualsense");
    kvText(buf, cap, &o, "controller.node", this->input_node);
    if (this->battery >= 0)
      kvAdd(buf, cap, &o, "controller.battery", "%d", this->battery);
    else
      kvText(buf, cap, &o, "controller.battery", "null");
    if (this->rssi_valid)
      kvAdd(buf, cap, &o, "controller.rssi", "%d", this->rssi);
    else
      kvText(buf, cap, &o, "controller.rssi", "null");
    if (this->link_quality >= 0)
      kvAdd(buf, cap, &o, "controller.link_quality", "%d", this->link_quality);
    else
      kvText(buf, cap, &o, "controller.link_quality", "null");
    if (this->tx_power >= 0)
      kvAdd(buf, cap, &o, "controller.tx_power", "%d", this->tx_power);
    else
      kvText(buf, cap, &o, "controller.tx_power", "null");
    if (this->afh_channels >= 0) {
      kvAdd(buf, cap, &o, "controller.afh_channels", "%d", this->afh_channels);
      kvAdd(buf, cap, &o, "controller.afh_mode", "%d", this->afh_mode);
      kvText(buf, cap, &o, "controller.afh_map", this->afh_map);
    } else {
      kvText(buf, cap, &o, "controller.afh_channels", "null");
      kvText(buf, cap, &o, "controller.afh_mode", "null");
      kvText(buf, cap, &o, "controller.afh_map", "");
    }
    if (this->supervision_ms >= 0)
      kvAdd(buf, cap, &o, "controller.supervision_ms", "%d", this->supervision_ms);
    else
      kvText(buf, cap, &o, "controller.supervision_ms", "null");
    if (this->link_policy >= 0)
      kvAdd(buf, cap, &o, "controller.link_policy", "%d", this->link_policy);
    else
      kvText(buf, cap, &o, "controller.link_policy", "null");
    kvText(buf, cap, &o, "controller.charging", this->charging);
    kvText(buf, cap, &o, "controller.power_state", this->powerState);
    kvAdd(buf, cap, &o, "controller.jack", "%s", this->jack ? "true" : "false");
    kvAdd(buf, cap, &o, "controller.plugged_usb_data", "%s",
          this->plugged_usb_data ? "true" : "false");
    kvAdd(buf, cap, &o, "controller.plugged_usb_power", "%s",
          this->pluggedUsbPower ? "true" : "false");
    kvAdd(buf, cap, &o, "controller.usb_host", "%s", this->externalHost() ? "true" : "false");
    kvAdd(buf, cap, &o, "controller.padwave_disabled", "%s",
          this->padwaveDisabled() ? "true" : "false");
    kvText(buf, cap, &o, "controller.buttons", this->buttons);
    kvText(buf, cap, &o, "controller.dpad", this->dpad);
    kvAdd(buf, cap, &o, "controller.rate", "%d", this->rate);
    kvAdd(buf, cap, &o, "controller.fields.host_timestamp", "%u", this->host_timestamp);
    kvAdd(buf, cap, &o, "controller.fields.device_timestamp", "%u", this->device_timestamp);
    kvAdd(buf, cap, &o, "controller.fields.trigger_effect_right", "%u", this->trigger_effect_r);
    kvAdd(buf, cap, &o, "controller.fields.trigger_effect_left", "%u", this->trigger_effect_l);
    for (int t = 0; t < 2; t++) {
      char key[48];
      snprintf(key, sizeof(key), "controller.touches.%d.active", t);
      kvAdd(buf, cap, &o, key, "%s", this->touch_active[t] ? "true" : "false");
      snprintf(key, sizeof(key), "controller.touches.%d.id", t);
      kvAdd(buf, cap, &o, key, "%u", this->touch_id[t]);
      snprintf(key, sizeof(key), "controller.touches.%d.x", t);
      kvAdd(buf, cap, &o, key, "%u", this->touch_x[t]);
      snprintf(key, sizeof(key), "controller.touches.%d.y", t);
      kvAdd(buf, cap, &o, key, "%u", this->touch_y[t]);
    }
    kvAdd(buf, cap, &o, "controller.l2", "%d", this->l2);
    kvAdd(buf, cap, &o, "controller.r2", "%d", this->r2);
    kvAdd(buf, cap, &o, "controller.l2_stop", "%u", this->trigger_stop_l);
    kvAdd(buf, cap, &o, "controller.r2_stop", "%u", this->trigger_stop_r);
    kvAdd(buf, cap, &o, "controller.l2_status", "%u", this->trigger_status_l);
    kvAdd(buf, cap, &o, "controller.r2_status", "%u", this->trigger_status_r);
    kvAdd(buf, cap, &o, "controller.l2_effect", "%u", this->trigger_effect_l);
    kvAdd(buf, cap, &o, "controller.r2_effect", "%u", this->trigger_effect_r);
    kvAdd(buf, cap, &o, "controller.lx", "%d", this->lx);
    kvAdd(buf, cap, &o, "controller.ly", "%d", this->ly);
    kvAdd(buf, cap, &o, "controller.rx", "%d", this->rx);
    kvAdd(buf, cap, &o, "controller.ry", "%d", this->ry);
    kvAdd(buf, cap, &o, "controller.temperature", "%d", this->temp);
    kvAdd(buf, cap, &o, "controller.accel", "%.3f,%.3f,%.3f", static_cast<double>(this->ax),
          static_cast<double>(this->ay), static_cast<double>(this->az));
    kvAdd(buf, cap, &o, "controller.gyro", "%.3f,%.3f,%.3f", static_cast<double>(this->gx),
          static_cast<double>(this->gy), static_cast<double>(this->gz));
    kvText(buf, cap, &o, "controller.raw", this->raw);
    kvText(buf, cap, &o, "controller.last_action", this->last_action);

    kvAdd(buf, cap, &o, "config.idle_timeout_ms", "%d", this->idle_timeout_ms);
    kvAdd(buf, cap, &o, "config.idle_warn_grace_ms", "%d", this->idle_warn_grace_ms);
    kvAdd(buf, cap, &o, "config.poweroff_when_idle_ms", "%d", this->poweroff_when_idle_ms);
    kvAdd(buf, cap, &o, "config.haptic_subwoofer", "%s", this->haptic_subwoofer ? "true" : "false");
    kvAdd(buf, cap, &o, "config.haptics_only", "%s", this->haptics_only ? "true" : "false");
    kvAdd(buf, cap, &o, "config.haptic_cutoff_hz", "%d", this->haptic_cutoff_hz);
    kvAdd(buf, cap, &o, "config.haptic_gain", "%d", this->haptic_gain);
    kvAdd(buf, cap, &o, "config.volume", "%d", this->volume);
    kvAdd(buf, cap, &o, "config.last_nonzero_volume", "%d", this->last_nonzero_volume);
    kvAdd(buf, cap, &o, "config.speed", "%d", this->speed);
    return static_cast<int>(o);
  }

  int frontText(char *buf, size_t cap, unsigned long requests, long uptime) {
    size_t o = 0;
    if (cap) buf[0] = 0;
    kvAdd(buf, cap, &o, "ok", "%s", "true");
    kvAdd(buf, cap, &o, "front", "%d", 1);
    kvAdd(buf, cap, &o, "uptime_s", "%ld", uptime);
    kvAdd(buf, cap, &o, "requests", "%lu", requests);
    kvAdd(buf, cap, &o, "controller", "%s", this->controller ? "true" : "false");
    kvAdd(buf, cap, &o, "player.valid", "%s", this->player ? "true" : "false");
    kvAdd(buf, cap, &o, "player.pos", "%ld", this->pos_ms);
    kvAdd(buf, cap, &o, "player.dur", "%ld", this->dur_ms);
    kvAdd(buf, cap, &o, "player.sent", "%ld", this->sent);
    kvAdd(buf, cap, &o, "player.underruns", "%ld", this->underruns);
    kvAdd(buf, cap, &o, "player.paused", "%s", this->paused ? "true" : "false");
    kvAdd(buf, cap, &o, "input.valid", "%s", this->input.fd() >= 0 ? "true" : "false");
    kvAdd(buf, cap, &o, "input.lx", "%d", this->lx);
    kvAdd(buf, cap, &o, "input.ly", "%d", this->ly);
    kvAdd(buf, cap, &o, "input.rx", "%d", this->rx);
    kvAdd(buf, cap, &o, "input.ry", "%d", this->ry);
    kvAdd(buf, cap, &o, "input.l2", "%d", this->l2);
    kvAdd(buf, cap, &o, "input.r2", "%d", this->r2);
    kvAdd(buf, cap, &o, "input.battery", "%d", this->battery);
    kvAdd(buf, cap, &o, "input.jack", "%s", this->jack ? "true" : "false");
    return static_cast<int>(o);
  }

  int configText(char *buf, size_t cap) {
    size_t o = 0;
    if (cap) buf[0] = 0;
    kvAdd(buf, cap, &o, "config.idle_timeout_ms", "%d", this->idle_timeout_ms);
    kvAdd(buf, cap, &o, "config.idle_warn_grace_ms", "%d", this->idle_warn_grace_ms);
    kvAdd(buf, cap, &o, "config.poweroff_when_idle_ms", "%d", this->poweroff_when_idle_ms);
    kvAdd(buf, cap, &o, "config.haptic_subwoofer", "%s", this->haptic_subwoofer ? "true" : "false");
    kvAdd(buf, cap, &o, "config.haptics_only", "%s", this->haptics_only ? "true" : "false");
    kvAdd(buf, cap, &o, "config.haptic_cutoff_hz", "%d", this->haptic_cutoff_hz);
    kvAdd(buf, cap, &o, "config.haptic_gain", "%d", this->haptic_gain);
    kvAdd(buf, cap, &o, "config.volume", "%d", this->volume);
    kvAdd(buf, cap, &o, "config.last_nonzero_volume", "%d", this->last_nonzero_volume);
    kvAdd(buf, cap, &o, "config.speed", "%d", this->speed);
    return static_cast<int>(o);
  }

  void micReset() {
    this->mic_packets = 0;
    this->mic_frames  = 0;
    this->mic_samples = 0;
    this->mic_rms     = 0;
    this->mic_peak    = 0;
    this->mic_level   = 0;
    this->mic_last_ms = 0;
  }

  void configSet(const char *body) {
    noteActivity();
    char value[64];

    if (kvGet(body, "idle_timeout_ms", value, sizeof(value))) {
      long v                = atol(value);
      this->idle_timeout_ms = static_cast<int>((v < 1000 ? 1000 : (v > 86400000 ? 86400000 : v)));
    }
    if (kvGet(body, "idle_warn_grace_ms", value, sizeof(value))) {
      long v                   = atol(value);
      this->idle_warn_grace_ms = static_cast<int>((v < 0 ? 0 : (v > 600000 ? 600000 : v)));
    }
    if (kvGet(body, "poweroff_when_idle_ms", value, sizeof(value))) {
      long v                      = atol(value);
      this->poweroff_when_idle_ms = static_cast<int>((v < 0 ? 0 : (v > 86400000 ? 86400000 : v)));
    }
    if (kvGet(body, "haptic_subwoofer", value, sizeof(value))) {
      this->haptic_subwoofer = strcmp(value, "true") == 0 || atoi(value) != 0;
    }
    if (kvGet(body, "haptics_only", value, sizeof(value))) {
      this->haptics_only = strcmp(value, "true") == 0 || atoi(value) != 0;
    }
    if (kvGet(body, "haptic_cutoff_hz", value, sizeof(value))) {
      int v                  = atoi(value);
      this->haptic_cutoff_hz = v < 20 ? 20 : (v > 2000 ? 2000 : v);
    }
    if (kvGet(body, "haptic_gain", value, sizeof(value))) {
      int v             = atoi(value);
      this->haptic_gain = v < 0 ? 0 : (v > 200 ? 200 : v);
    }
    if (kvGet(body, "volume", value, sizeof(value))) setVolume(atoi(value));
    if (this->player) {
      char line[64];
      snprintf(line, sizeof(line), "haptic %d %d %d %d\n", this->haptic_subwoofer ? 1 : 0,
               this->haptic_cutoff_hz, this->haptic_gain, this->haptics_only ? 1 : 0);
      playerSend(line);
    }
    configSave();
    this->volume_dirty = false;
  }

  bool libraryPath(char *buf, size_t cap) const { return cachePath("library.tsv", buf, cap); }

  int libraryText(char *buf, size_t cap) {
    size_t o = 0;
    if (cap) buf[0] = 0;
    pthread_mutex_lock(&library_mutex);
    for (int i = 0; i < this->count; i++) {
      const Track *s = &this->tracks[i];
      char duration[32];
      snprintf(duration, sizeof(duration), "%lld", static_cast<long long>(s->duration_ms));
      tsvField(buf, cap, &o, s->id, '\t');
      tsvField(buf, cap, &o, s->title, '\t');
      tsvField(buf, cap, &o, s->folder, '\t');
      tsvField(buf, cap, &o, s->path, '\t');
      tsvField(buf, cap, &o, s->ext, '\t');
      tsvField(buf, cap, &o, duration, '\t');
      tsvField(buf, cap, &o, s->favorite ? "1" : "0", '\n');
    }
    pthread_mutex_unlock(&library_mutex);
    return static_cast<int>(o);
  }

  int liveText(char *buf, size_t cap) {
    size_t o = 0;
    if (cap) buf[0] = 0;
    for (int i = 0; i < this->live_count; i++) {
      char id[16];
      snprintf(id, sizeof(id), "%d", i + 1);
      const LiveChannel *c = &this->live_channels[i];
      tsvField(buf, cap, &o, id, '\t');
      tsvField(buf, cap, &o, c->number, '\t');
      tsvField(buf, cap, &o, c->name, '\t');
      tsvField(buf, cap, &o, c->group, '\t');
      tsvField(buf, cap, &o, c->logo, '\t');
      tsvField(buf, cap, &o, c->country, '\t');
      tsvField(buf, cap, &o, c->provider, '\t');
      tsvField(buf, cap, &o, c->language, '\t');
      tsvField(buf, cap, &o, c->radio ? "true" : "false", '\t');
      tsvField(buf, cap, &o, c->url, '\n');
    }
    return static_cast<int>(o);
  }

  // Resolve a track id (or the current track) to its path and browser MIME type.
  // The path remains owned by App and is only valid until the next library scan.
  bool streamTrack(const char *id, const char **path, const char **mime) const {
    int index = (id && *id) ? findId(id) : this->current;
    if (index < 0 || index >= this->count) return false;
    const Track *track = &this->tracks[index];
    *path              = track->path;
    const char *ext    = track->ext;
    if (strcmp(ext, "mp3") == 0)
      *mime = "audio/mpeg";
    else if (strcmp(ext, "m4a") == 0 || strcmp(ext, "aac") == 0)
      *mime = "audio/mp4";
    else if (strcmp(ext, "flac") == 0)
      *mime = "audio/flac";
    else if (strcmp(ext, "wav") == 0)
      *mime = "audio/wav";
    else if (strcmp(ext, "ogg") == 0 || strcmp(ext, "opus") == 0)
      *mime = "audio/ogg";
    else
      *mime = "application/octet-stream";
    return true;
  }

  static void pcmTap(void *owner, const float *pcm, int frames) {
    static_cast<App *>(owner)->webPcm(pcm, frames);
  }

  void webPcm(const float *pcm, int frames) {
    if (this->web_audio_clients <= 0 || !pcm || frames <= 0) return;
    constexpr size_t cap = sizeof(this->web_pcm);
    size_t bytes = static_cast<size_t>(frames) * CHANNELS * sizeof(int16_t);
    size_t offset = static_cast<size_t>(this->web_pcm_head % cap);
    size_t first = bytes < cap - offset ? bytes : cap - offset;
    int16_t *out = reinterpret_cast<int16_t *>(this->web_pcm + offset);
    for (size_t i = 0; i < first / sizeof(int16_t); i += CHANNELS) {
      size_t frame = i / CHANNELS;
      for (size_t ch = 0; ch < static_cast<size_t>(CHANNELS); ch++) {
        float v = pcm[frame * CHANNELS + ch];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        out[i + ch] = static_cast<int16_t>(v * 32767.0f);
      }
    }
    if (first < bytes) {
      size_t rest = bytes - first;
      int16_t *second = reinterpret_cast<int16_t *>(this->web_pcm);
      for (size_t i = 0; i < rest / sizeof(int16_t); i += CHANNELS) {
        size_t frame = first / sizeof(int16_t) / CHANNELS + i / CHANNELS;
        for (size_t ch = 0; ch < static_cast<size_t>(CHANNELS); ch++) {
          float v = pcm[frame * CHANNELS + ch];
          if (v > 1.0f) v = 1.0f;
          if (v < -1.0f) v = -1.0f;
          second[i + ch] = static_cast<int16_t>(v * 32767.0f);
        }
      }
    }
    this->web_pcm_head += bytes;
  }

  uint64_t browserAudioHead() const { return this->web_pcm_head; }

  size_t browserAudioReadAt(uint64_t pos, char *out, size_t cap, uint64_t *next) {
    constexpr uint64_t ring = sizeof(this->web_pcm);
    if (this->web_pcm_head <= pos) return 0;
    if (this->web_pcm_head - pos > ring) pos = this->web_pcm_head - ring;
    size_t n = static_cast<size_t>(this->web_pcm_head - pos);
    if (n > cap) n = cap;
    n &= ~static_cast<size_t>(1);
    if (n == 0) return 0;
    size_t offset = static_cast<size_t>(pos % ring);
    size_t first = n < ring - offset ? n : static_cast<size_t>(ring - offset);
    memcpy(out, this->web_pcm + offset, first);
    memcpy(out + first, this->web_pcm, n - first);
    *next = pos + n;
    return n;
  }

  void browserAudioAddClient() { this->web_audio_clients++; }

  void browserAudioRemoveClient() {
    if (this->web_audio_clients > 0) this->web_audio_clients--;
  }

  int output(const char *body, char *out, size_t out_cap) {
    char kind[32];
    if (!kvGet(body, "kind", kind, sizeof(kind)))
      return snprintf(out, out_cap, "ok=false\nerror=missing kind\n");
    if (!this->player) return snprintf(out, out_cap, "ok=false\nerror=no player\n");

    char fields[1024];

    if (strcmp(kind, "lightbar") == 0) {
      reclaimLeds();
      char rgb[64] = "0,0,0";
      int r = 0, g = 0, b = 0;
      kvGet(body, "rgb", rgb, sizeof(rgb));
      sscanf(rgb, "%d,%d,%d", &r, &g, &b);
      r                    = r < 0 ? 0 : (r > 255 ? 255 : r);
      g                    = g < 0 ? 0 : (g > 255 ? 255 : g);
      b                    = b < 0 ? 0 : (b > 255 ? 255 : b);
      this->led_custom[0]  = r;
      this->led_custom[1]  = g;
      this->led_custom[2]  = b;
      this->led_custom_set = true;
      this->lightbar_on    = true;
      this->led_force      = true;
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=lightbar %d,%d,%d\n", r, g, b);
    }

    if (strcmp(kind, "lightbar_reset") == 0) {
      reclaimLeds();
      this->led_custom_set = false;
      this->lightbar_on    = true;
      this->led_force      = true;
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=lightbar reset\n");
    }

    if (strcmp(kind, "lightbar_fade") == 0) {
      reclaimLeds();
      this->lightbar_on = false;
      this->led_force   = true;
      setLightbarSetup(dualsense::OutputBody::LightbarSetup::LightOut);
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=light off\n");
    }

    if (strcmp(kind, "lightbar_setup") == 0) {
      reclaimLeds();
      int v = static_cast<int>(kvNumber(body, "value", 0));
      if (v != 1 && v != 2) return snprintf(out, out_cap, "ok=false\nerror=setup must be 1 or 2\n");
      auto setup        = static_cast<dualsense::OutputBody::LightbarSetup>(v);
      this->lightbar_on = setup == dualsense::OutputBody::LightbarSetup::LightOn;
      this->led_force   = true;
      setLightbarSetup(setup);
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=setup %d\n", v);
    }

    if (strcmp(kind, "audio_control") == 0) {
      int value = static_cast<int>(kvNumber(body, "value", 0));
      if (value < 0 || value > 255)
        return snprintf(out, out_cap, "ok=false\nerror=value must be 0..255\n");
      if (!this->player.setAudioControl(static_cast<uint8_t>(value)))
        return snprintf(out, out_cap, "ok=false\nerror=audio control write failed\n");
      return snprintf(out, out_cap, "ok=true\nnote=0x%02x %s\n", static_cast<unsigned>(value),
                      audioPathName(this->player.getAudioPath()));
    }

    if (strcmp(kind, "audio_control_auto") == 0) {
      applyOutputRoute();
      return snprintf(out, out_cap, "ok=true\nnote=%s\n",
                      audioPathName(this->player.getAudioPath()));
    }

    if (strcmp(kind, "player_led_brightness") == 0) {
      reclaimLeds();
      if (this->padwaveDisabled())
        return snprintf(out, out_cap, "ok=true\nnote=controller disabled\n");
      int value = static_cast<int>(kvNumber(body, "value", 0));
      if (value < 0 || value > 2)
        return snprintf(out, out_cap, "ok=false\nerror=brightness must be 0..2\n");
      snprintf(fields, sizeof(fields), "player_led_brightness=%d", value);
      outputFields(fields);
      return snprintf(out, out_cap, "ok=true\nnote=player brightness %d\n", value);
    }

    if (strcmp(kind, "controller_input") == 0) {
      if (!this->plugged_usb_data)
        return snprintf(out, out_cap, "ok=false\nerror=no USB data link\n");
      int enabled               = static_cast<int>(kvNumber(body, "enabled", 1));
      this->controller_disabled = enabled == 0;
      if (this->controller_disabled) playerSend("state_clear_lightbar\n");
      this->led_force = true;
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=controller %s\n",
                      this->controller_disabled ? "ignored" : "active");
    }

    if (strcmp(kind, "release") == 0) {
      if (!this->player) return snprintf(out, out_cap, "ok=false\nerror=no player\n");
      bool ok            = this->player.setReleaseLEDs();
      this->led_released = ok;
      return snprintf(out, out_cap, "ok=%s\nnote=LED control released\n", ok ? "true" : "false");
    }

    if (strcmp(kind, "player_leds") == 0) {
      reclaimLeds();
      int mask                = static_cast<int>(kvNumber(body, "mask", 0)) & 0x3F;
      this->led_manual_player = true;
      this->led_manual_mask   = mask;
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=player leds %d\n", mask);
    }

    if (strcmp(kind, "power_save") == 0) {
      if (this->power_save_mode) setPowerSaveMode(false);
      char bits[64] = "";
      kvGet(body, "bits", bits, sizeof(bits));
      int mask   = 0;
      char *save = nullptr;
      for (char *tok = strtok_r(bits, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        int bit = atoi(tok);
        if (bit >= 0 && bit < 8) mask |= 1 << bit;
      }
      this->power_save_mask = mask;
      snprintf(fields, sizeof(fields), "power_save_control=%d", mask);
      outputFields(fields);
      return snprintf(out, out_cap, "ok=true\nnote=power save 0x%02x\n",
                      static_cast<unsigned>(mask));
    }

    if (strcmp(kind, "power_save_mode") == 0) {
      int enabled = static_cast<int>(kvNumber(body, "enabled", 0));
      setPowerSaveMode(enabled != 0);
      return snprintf(out, out_cap, "ok=true\nnote=power save mode %s\n",
                      this->power_save_mode ? "on" : "off");
    }

    if (strcmp(kind, "rumble") == 0) {
      int left  = static_cast<int>(kvNumber(body, "left", 0));
      int right = static_cast<int>(kvNumber(body, "right", 0));
      long ms   = kvNumber(body, "ms", 0);
      left      = left < 0 ? 0 : (left > 255 ? 255 : left);
      right     = right < 0 ? 0 : (right > 255 ? 255 : right);
      // The main motors need both ValidFlag0 bits: Rumble (0x01) and Haptics
      // (0x02). DS4Windows and dualsense-tester both set the pair; bit 0 alone
      // is ignored by the controller.
      snprintf(fields, sizeof(fields), "motor_left=%d;motor_right=%d", left, right);
      outputFields(fields);
      this->idle_rumble_until = 0;
      this->rumble_left       = left;
      this->rumble_right      = right;
      this->rumble_until      = ms > 0 ? nowMs() + ms : 0;
      return snprintf(out, out_cap, "ok=true\nnote=rumble %d/%d\n", left, right);
    }

    if (strcmp(kind, "trigger") == 0 || strcmp(kind, "left_trigger") == 0 ||
        strcmp(kind, "right_trigger") == 0) {
      char side[16]   = "right";
      char effect[32] = "vibration";
      if (strcmp(kind, "left_trigger") == 0)
        snprintf(side, sizeof(side), "left");
      else if (strcmp(kind, "trigger") == 0)
        kvGet(body, "side", side, sizeof(side));
      kvGet(body, "effect", effect, sizeof(effect));
      int position    = static_cast<int>(kvNumber(body, "position", 2));
      int amplitude   = static_cast<int>(kvNumber(body, "amplitude", 6));
      int frequency   = static_cast<int>(kvNumber(body, "frequency", 25));
      int wall        = static_cast<int>(kvNumber(body, "wall", 8));
      int start       = static_cast<int>(kvNumber(body, "start", position));
      int end         = static_cast<int>(kvNumber(body, "end", wall));
      int level       = static_cast<int>(kvNumber(body, "level", amplitude));
      int amplitude_b = static_cast<int>(kvNumber(body, "amplitude_b", 0));
      int period      = static_cast<int>(kvNumber(body, "period", 0));

      using TriggerEffect = dualsense::OutputBody::TriggerEffect;
      TriggerEffect fx    = TriggerEffect::off();
      char label[40]      = "off";
      bool off            = strcmp(effect, "off") == 0 || strcmp(effect, "none") == 0;
      if (!off) {
        const uint8_t level8     = static_cast<uint8_t>(level);
        const uint8_t frequency8 = static_cast<uint8_t>(frequency);
        if (strcmp(effect, "feedback") == 0) {
          uint8_t levels[10] = {};
          int last           = end < start ? 9 : (end > 9 ? 9 : end);
          for (int zone = start < 0 ? 0 : (start > 9 ? 9 : start); zone <= last; ++zone)
            levels[zone] = level8;
          fx = TriggerEffect::multiplePositionFeedback(levels);
        } else if (strcmp(effect, "weapon") == 0) {
          fx =
              TriggerEffect::weapon(static_cast<uint8_t>(start), static_cast<uint8_t>(end), level8);
        } else if (strcmp(effect, "vibration") == 0) {
          uint8_t amplitudes[10] = {};
          int last               = end < start ? 9 : (end > 9 ? 9 : end);
          for (int zone = start < 0 ? 0 : (start > 9 ? 9 : start); zone <= last; ++zone)
            amplitudes[zone] = level8;
          fx = TriggerEffect::multiplePositionVibration(frequency8, amplitudes);
        } else if (strcmp(effect, "machine") == 0) {
          fx = TriggerEffect::machine(static_cast<uint8_t>(start), static_cast<uint8_t>(end),
                                      level8, static_cast<uint8_t>(amplitude_b), frequency8,
                                      static_cast<uint8_t>(period));
        } else if (strcmp(effect, "bow") == 0) {
          fx = TriggerEffect::bow(static_cast<uint8_t>(start), static_cast<uint8_t>(end), level8,
                                  static_cast<uint8_t>(amplitude_b));
        } else if (strcmp(effect, "galloping") == 0) {
          fx = TriggerEffect::galloping(static_cast<uint8_t>(start), static_cast<uint8_t>(end),
                                        level8, static_cast<uint8_t>(amplitude_b), frequency8);
        } else if (strcmp(effect, "slope") == 0) {
          fx = TriggerEffect::slopeFeedback(static_cast<uint8_t>(start), static_cast<uint8_t>(end),
                                            level8, static_cast<uint8_t>(amplitude_b));
        } else if (strcmp(effect, "simple-feedback") == 0) {
          fx = TriggerEffect::simpleFeedback(static_cast<uint8_t>(start), level8);
        } else if (strcmp(effect, "simple-weapon") == 0) {
          fx = TriggerEffect::simpleWeapon(static_cast<uint8_t>(start), static_cast<uint8_t>(end),
                                           level8);
        } else if (strcmp(effect, "simple-vibration") == 0) {
          fx = TriggerEffect::simpleVibration(static_cast<uint8_t>(start), level8, frequency8);
        } else if (strcmp(effect, "limited-feedback") == 0) {
          fx = TriggerEffect::limitedFeedback(static_cast<uint8_t>(start), level8);
        } else if (strcmp(effect, "limited-weapon") == 0) {
          fx = TriggerEffect::limitedWeapon(static_cast<uint8_t>(start), static_cast<uint8_t>(end),
                                            level8);
        } else if (strcmp(effect, "rewasd-full") == 0) {
          fx = TriggerEffect::rewasdFullPress();
        } else if (strcmp(effect, "rewasd-soft") == 0) {
          fx = TriggerEffect::rewasdSoftPress();
        } else if (strcmp(effect, "rewasd-medium") == 0) {
          fx = TriggerEffect::rewasdMediumPress();
        } else if (strcmp(effect, "rewasd-hard") == 0) {
          fx = TriggerEffect::rewasdHardPress();
        } else if (strcmp(effect, "rewasd-pulse") == 0) {
          fx = TriggerEffect::rewasdPulse();
        } else if (strcmp(effect, "rewasd-choppy") == 0) {
          fx = TriggerEffect::rewasdChoppy();
        } else if (strcmp(effect, "rewasd-soft-rigidity") == 0) {
          fx = TriggerEffect::rewasdSoftRigidity();
        } else if (strcmp(effect, "rewasd-medium-rigidity") == 0) {
          fx = TriggerEffect::rewasdMediumRigidity();
        } else if (strcmp(effect, "rewasd-max-rigidity") == 0) {
          fx = TriggerEffect::rewasdMaxRigidity();
        } else if (strcmp(effect, "rewasd-half-press") == 0) {
          fx = TriggerEffect::rewasdHalfPress();
        } else if (strcmp(effect, "rewasd-rifle") == 0) {
          fx = TriggerEffect::rewasdRifle(frequency8);
        } else if (strcmp(effect, "rewasd-vibration") == 0) {
          fx = TriggerEffect::rewasdVibration(level8, frequency8);
        } else {
          return snprintf(out, out_cap, "ok=false\nerror=unknown trigger effect\n");
        }
        if (!fx.isValid()) {
          fx  = TriggerEffect::off();
          off = true;
        }
        snprintf(label, sizeof(label), "%s", effect);
      }

      if (!this->player) return snprintf(out, out_cap, "ok=false\nerror=no player\n");

      bool left = strcmp(side, "left") == 0;
      this->player.setTriggerEffect(left, fx);
      return snprintf(out, out_cap, "ok=true\nnote=trigger %s %s\n", side, label);
    }

    if (strcmp(kind, "firmware_info") == 0) {
      if (!this->player) return snprintf(out, out_cap, "ok=false\nerror=no player\n");
      char info[1024];
      int n = this->player.firmwareInfo(info, sizeof(info));
      if (n <= 0) return snprintf(out, out_cap, "ok=false\nerror=firmware read failed\n");
      return snprintf(out, out_cap, "%s", info);
    }

    if (strcmp(kind, "extra_info") == 0) {
      if (!this->player) return snprintf(out, out_cap, "ok=false\nerror=no player\n");
      char info[2048];
      int n = this->player.extraInfo(info, sizeof(info));
      if (n <= 0) return snprintf(out, out_cap, "ok=false\nerror=extra read failed\n");
      return snprintf(out, out_cap, "%s", info);
    }

    if (strcmp(kind, "fields") == 0) {
      if (!kvGet(body, "fields", fields, sizeof(fields)))
        return snprintf(out, out_cap, "ok=false\nerror=missing fields\n");
      outputFields(fields);
      return snprintf(out, out_cap, "ok=true\nnote=fields\n");
    }

    if (strcmp(kind, "mic_monitor") == 0) {
      char value[16] = "false";
      kvGet(body, "enabled", value, sizeof(value));
      setMicMonitor(strcmp(value, "true") == 0 || atoi(value) != 0);
      return snprintf(out, out_cap, "ok=true\nnote=mic monitor %s\n",
                      this->mic_monitor ? "on" : "off");
    }

    return snprintf(out, out_cap, "ok=false\nerror=unknown kind\n");
  }

  int handleCommand(const char *body, size_t len, char *out, size_t cap) {
    char action[64];
    if (!kvGet(body, "action", action, sizeof(action)))
      return snprintf(out, cap, "ok=false\nerror=missing action\n");
    return command(action, body, len, out, cap);
  }

  bool ownsFd(int fd) {
    return fd >= 0 &&
           (fd == this->player_timer_fd || fd == this->input.fd() || fd == this->sink.fd());
  }

private:
  void addTrack(const char *path, const char *folder, const struct stat &st) {
    if (this->count >= kMaxTracks) return;
    Track *s = &this->tracks[this->count];
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->size       = static_cast<int64_t>(st.st_size);
    s->mtime_sec  = static_cast<int64_t>(st.st_mtim.tv_sec);
    s->mtime_nsec = st.st_mtim.tv_nsec;

    const char *base = strrchr(path, '/');
    snprintf(s->title, sizeof(s->title), "%.*s", static_cast<int>(sizeof(s->title)) - 1,
             base ? base + 1 : path);
    char *dot = strrchr(s->title, '.');
    if (dot) *dot = 0;

    snprintf(s->folder, sizeof(s->folder), "%.*s", static_cast<int>(sizeof(s->folder)) - 1, folder);
    const char *d = strrchr(path, '.');
    snprintf(s->ext, sizeof(s->ext), "%s", d ? d + 1 : "");
    makeId(path, s->id);
    this->count++;
  }

  static bool validLiveUrl(const char *url) {
    if (!url || !*url) return false;
    size_t n = strlen(url);
    if (n >= kMaxLiveUrl) return false;
    if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0)
      return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(url); *p; p++)
      if (*p <= 0x20 || *p == 0x7f) return false;
    return true;
  }

  static bool validProxyUrl(const char *url) {
    if (!url || !*url) return true;
    if (strlen(url) >= 512) return false;
    const char *schemes[] = {"http://", "https://", "socks4://", "socks4a://",
                             "socks5://", "socks5h://"};
    bool ok = false;
    for (const char *scheme : schemes)
      if (strncasecmp(url, scheme, strlen(scheme)) == 0) ok = true;
    if (!ok) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(url); *p; p++)
      if (*p <= 0x20 || *p == 0x7f) return false;
    return true;
  }

  static char *trimLine(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
  }

  static bool m3uAttr(const char *line, const char *key, char *out, size_t cap) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(line, needle);
    if (!p) return false;
    p += strlen(needle);
    const char *e;
    if (*p == '"') {
      p++;
      e = strchr(p, '"');
    } else {
      e = p;
      while (*e && *e != ' ' && *e != '\t') e++;
    }
    if (!e) return false;
    size_t n = static_cast<size_t>(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
  }

  void addLiveChannel(const char *name, const char *group, const char *logo, const char *number,
                      const char *country, const char *provider, const char *language, bool radio,
                      const char *url) {
    if (this->live_count >= kMaxLiveChannels || !url || !*url) return;
    LiveChannel *c = &this->live_channels[this->live_count];
    snprintf(c->name, sizeof(c->name), "%s", name && *name ? name : url);
    snprintf(c->group, sizeof(c->group), "%s", group ? group : "");
    snprintf(c->logo, sizeof(c->logo), "%s", logo ? logo : "");
    snprintf(c->number, sizeof(c->number), "%s", number ? number : "");
    snprintf(c->country, sizeof(c->country), "%s", country ? country : "");
    snprintf(c->provider, sizeof(c->provider), "%s", provider ? provider : "");
    snprintf(c->language, sizeof(c->language), "%s", language ? language : "");
    snprintf(c->url, sizeof(c->url), "%s", url);
    c->radio = radio;
    this->live_count++;
  }

  void parsePlaylist(char *body) {
    this->live_count = 0;
    char group[kMaxFolder] = "";
    char name[kMaxTitle]   = "";
    char logo[256]         = "";
    char number[16]        = "";
    char country[32]       = "";
    char provider[64]      = "";
    char language[32]      = "";
    bool radio             = false;
    bool haveInfo          = false;
    char *save             = nullptr;

    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
      line = trimLine(line);
      if (!*line) continue;
      if (strncmp(line, "#EXTGRP:", 8) == 0) {
        snprintf(group, sizeof(group), "%s", trimLine(line + 8));
        continue;
      }
      if (strncmp(line, "#EXTINF:", 8) == 0) {
        m3uAttr(line, "group-title", group, sizeof(group));
        m3uAttr(line, "tvg-logo", logo, sizeof(logo));
        m3uAttr(line, "tvg-chno", number, sizeof(number));
        if (!m3uAttr(line, "provider-countries", country, sizeof(country)))
          m3uAttr(line, "tvg-country", country, sizeof(country));
        m3uAttr(line, "provider", provider, sizeof(provider));
        m3uAttr(line, "provider-languages", language, sizeof(language));
        char radio_text[16];
        radio = m3uAttr(line, "radio", radio_text, sizeof(radio_text)) &&
                (strcasecmp(radio_text, "true") == 0 || strcmp(radio_text, "1") == 0);
        const char *comma = strrchr(line, ',');
        name[0]           = 0;
        if (comma && comma[1]) snprintf(name, sizeof(name), "%s", trimLine(const_cast<char *>(comma + 1)));
        haveInfo = true;
        continue;
      }
      if (*line == '#') continue;
      if (haveInfo) {
        if (validLiveUrl(line))
          this->addLiveChannel(name, group, logo, number, country, provider, language, radio, line);
        haveInfo = false;
      }
    }

    // A direct HLS playlist has no #EXTINF entries; treat it as one stream.
    if (this->live_count == 0 && strstr(this->live_url, ".m3u8"))
      this->addLiveChannel("Stream", "", "", "", "", "", "", false, this->live_url);
  }

  static bool liveCachePath(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    snprintf(buf, cap, "%s/.cache/padwave/live.m3u", home);
    return true;
  }

  void saveLiveCache(const char *body, size_t len) {
    char path[512];
    if (!liveCachePath(path, sizeof(path))) return;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
      *slash = 0;
      mkdir(dir, 0755);
    }
    FILE *f = fopen(path, "w");
    if (!f) return;
    fwrite(body, 1, len, f);
    fclose(f);
  }

  bool loadLiveCache() {
    char path[512];
    if (!liveCachePath(path, sizeof(path))) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    constexpr size_t kMaxBody = 8 * 1024 * 1024;
    char *body = static_cast<char *>(malloc(kMaxBody));
    if (!body) {
      fclose(f);
      return false;
    }
    size_t n = fread(body, 1, kMaxBody - 1, f);
    fclose(f);
    body[n] = 0;
    if (n > 0) this->parsePlaylist(body);
    free(body);
    return this->live_count > 0;
  }

  struct CurlWrite {
    char *body;
    size_t cap;
    size_t len;
    bool overflow;
  };

  static size_t curlWrite(char *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlWrite *w = static_cast<CurlWrite *>(userdata);
    size_t n     = size * nmemb;
    if (w->len + n >= w->cap) {
      w->overflow = true;
      return 0;
    }
    memcpy(w->body + w->len, ptr, n);
    w->len += n;
    return n;
  }

  static size_t curlFileWrite(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return fwrite(ptr, size, nmemb, static_cast<FILE *>(userdata));
  }

  void applyCurlProxy(CURL *h, bool use_proxy = true) const {
    if (this->live_insecure) {
      curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (!use_proxy || !this->live_proxy[0]) return;
    // socks5:// makes libcurl resolve DNS locally; socks5h:// sends the
    // hostname to the proxy. Prefer remote resolution so DNS-blocked names
    // work through the proxy. Same for SOCKS4a.
    const char *proxy = this->live_proxy;
    char normalized[600];
    if (strncasecmp(proxy, "socks5://", 9) == 0) {
      snprintf(normalized, sizeof(normalized), "socks5h://%s", proxy + 9);
      proxy = normalized;
    } else if (strncasecmp(proxy, "socks4://", 9) == 0) {
      snprintf(normalized, sizeof(normalized), "socks4a://%s", proxy + 9);
      proxy = normalized;
    }
    curl_easy_setopt(h, CURLOPT_PROXY, proxy);
    if (this->live_proxy_user[0]) {
      char cred[300];
      snprintf(cred, sizeof(cred), "%s:%s", this->live_proxy_user, this->live_proxy_pass);
      curl_easy_setopt(h, CURLOPT_PROXYUSERPWD, cred);
    }
  }

  int fetchUrl(const char *url, char *body, size_t cap) const {
    if (!body || cap == 0) return -1;
    bool has_proxy = this->live_proxy[0];
    for (int attempt = 0; attempt < 2; attempt++) {
      if (attempt == 1 && !has_proxy) break;
      body[0] = 0;
      CURL *h = curl_easy_init();
      if (!h) return -1;
      CurlWrite write{body, cap, 0, false};
      curl_easy_setopt(h, CURLOPT_URL, url);
      curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
      curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
      curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 20000L);
      curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(h, CURLOPT_USERAGENT, "padwave/1");
      curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curlWrite);
      curl_easy_setopt(h, CURLOPT_WRITEDATA, &write);
      curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(16 * 1024 * 1024));
      curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
      curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
      this->applyCurlProxy(h, attempt == 1);
      CURLcode rc = curl_easy_perform(h);
      long code   = 0;
      curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
      curl_easy_cleanup(h);
      body[write.len] = 0;
      if (rc == CURLE_OK && !write.overflow && code >= 200 && code < 300)
        return static_cast<int>(write.len);
    }
    return -1;
  }

  bool fetchUrlToFile(const char *url, const char *path) const {
    bool has_proxy = this->live_proxy[0];
    for (int attempt = 0; attempt < 2; attempt++) {
      if (attempt == 1 && !has_proxy) break;
      FILE *f = fopen(path, "wb");
      if (!f) return false;
      CURL *h = curl_easy_init();
      if (!h) {
        fclose(f);
        unlink(path);
        return false;
      }
      curl_easy_setopt(h, CURLOPT_URL, url);
      curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
      curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
      curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 30000L);
      curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(h, CURLOPT_USERAGENT, "padwave/1");
      curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curlFileWrite);
      curl_easy_setopt(h, CURLOPT_WRITEDATA, f);
      curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(64 * 1024 * 1024));
      curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
      curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
      this->applyCurlProxy(h, attempt == 1);
      CURLcode rc = curl_easy_perform(h);
      long code   = 0;
      curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
      curl_easy_cleanup(h);
      fclose(f);
      if (rc == CURLE_OK && code >= 200 && code < 300) return true;
      unlink(path);
    }
    return false;
  }

  bool refreshLive() {
    this->live_error[0] = 0;
    if (!validLiveUrl(this->live_url)) {
      snprintf(this->live_error, sizeof(this->live_error), "set an http(s) M3U/M3U8 URL first");
      return false;
    }
    constexpr size_t kMaxBody = 8 * 1024 * 1024;
    char *body                = static_cast<char *>(malloc(kMaxBody));
    if (!body) {
      snprintf(this->live_error, sizeof(this->live_error), "out of memory");
      return false;
    }
    int n = fetchUrl(this->live_url, body, kMaxBody);
    if (n > 0) saveLiveCache(body, static_cast<size_t>(n));
    if (n <= 0) {
      snprintf(this->live_error, sizeof(this->live_error), "could not fetch playlist");
      free(body);
      return false;
    }
    this->parsePlaylist(body);
    free(body);
    if (this->live_count == 0) {
      snprintf(this->live_error, sizeof(this->live_error), "playlist has no playable entries");
      return false;
    }
    return true;
  }

  static bool urlEncode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(in); *p; p++) {
      if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
        if (o + 1 >= cap) return false;
        out[o++] = static_cast<char>(*p);
      } else {
        if (o + 3 >= cap) return false;
        out[o++] = '%';
        out[o++] = hex[*p >> 4];
        out[o++] = hex[*p & 15];
      }
    }
    out[o] = 0;
    return true;
  }

  static bool urlDecode(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p; p++) {
      unsigned char c;
      if (*p == '%' && p[1] && p[2]) {
        auto digit = [](char ch) -> int {
          if (ch >= '0' && ch <= '9') return ch - '0';
          if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
          if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
          return -1;
        };
        int hi = digit(p[1]), lo = digit(p[2]);
        if (hi < 0 || lo < 0) return false;
        c = static_cast<unsigned char>((hi << 4) | lo);
        p += 2;
      } else {
        c = static_cast<unsigned char>(*p == '+' ? ' ' : *p);
      }
      if (o + 1 >= cap) return false;
      out[o++] = static_cast<char>(c);
    }
    out[o] = 0;
    return true;
  }

  static bool appendText(char *out, size_t cap, size_t *o, const char *text) {
    size_t n = strlen(text);
    if (*o + n >= cap) return false;
    memcpy(out + *o, text, n);
    *o += n;
    out[*o] = 0;
    return true;
  }

  int rewritePlaylistBody(char *body, char *out, size_t cap) {
    size_t segments = 0;
    for (const char *p = body; *p;) {
      const char *e = strchr(p, '\n');
      size_t len    = e ? static_cast<size_t>(e - p) : strlen(p);
      size_t start  = 0;
      while (start < len && (p[start] == ' ' || p[start] == '\t' || p[start] == '\r')) start++;
      if (start < len && p[start] != '#') segments++;
      if (!e) break;
      p = e + 1;
    }
    constexpr size_t kKeep = 8;
    size_t keep_from       = segments > kKeep ? segments - kKeep : 0;

    size_t o = 0;
    char pending[512] = "";
    size_t segment_index = 0;
    char *save = nullptr;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
      line = trimLine(line);
      if (!*line) continue;
      if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
        long seq = strtol(line + 22, nullptr, 10);
        char adjusted[64];
        snprintf(adjusted, sizeof(adjusted), "#EXT-X-MEDIA-SEQUENCE:%ld", seq + static_cast<long>(keep_from));
        if (!appendText(out, cap, &o, adjusted) || !appendText(out, cap, &o, "\n")) break;
        continue;
      }
      if (strncmp(line, "#EXTINF:", 8) == 0) {
        snprintf(pending, sizeof(pending), "%s", line);
        continue;
      }
      if (*line == '#') {
        if (!appendText(out, cap, &o, line) || !appendText(out, cap, &o, "\n")) break;
        continue;
      }
      if (segment_index >= keep_from) {
        if (pending[0]) {
          if (!appendText(out, cap, &o, pending) || !appendText(out, cap, &o, "\n")) break;
          pending[0] = 0;
        }
        char absolute[kMaxPath];
        resolveRelative(this->live_upstream_url, line, absolute, sizeof(absolute));
        char encoded[kMaxPath * 3];
        if (!urlEncode(absolute, encoded, sizeof(encoded))) break;
        char rewritten[kMaxPath * 3 + 32];
        snprintf(rewritten, sizeof(rewritten), "/hls-src/seg?u=%s\n", encoded);
        if (!appendText(out, cap, &o, rewritten)) break;
      } else {
        pending[0] = 0;
      }
      segment_index++;
    }
    return static_cast<int>(o);
  }

  int liveSourcePlaylist(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!this->live_upstream_url[0]) return 0;
    char resolved[kMaxPath];
    resolveLiveStream(this->live_upstream_url, resolved, sizeof(resolved));
    if (resolved[0] && strcmp(resolved, this->live_upstream_url) != 0)
      snprintf(this->live_upstream_url, sizeof(this->live_upstream_url), "%s", resolved);
    constexpr size_t kMaxBody = kPlaylistBytes;
    char *body = static_cast<char *>(malloc(kMaxBody));
    if (!body) return 0;
    int n = fetchUrl(this->live_upstream_url, body, kMaxBody);
    if (n <= 0) {
      free(body);
      return 0;
    }

    int result = this->rewritePlaylistBody(body, out, cap);
    free(body);
    return result;
  }

  void writeLivePlaylistFile() {
    if (!this->live_pl_buf.data || this->live_pl_buf.len == 0) return;
    constexpr size_t kOutCap = 4 * 1024 * 1024;
    char *copy = static_cast<char *>(malloc(this->live_pl_buf.len + 1));
    char *out  = static_cast<char *>(malloc(kOutCap));
    if (!copy || !out) {
      free(copy);
      free(out);
      return;
    }
    memcpy(copy, this->live_pl_buf.data, this->live_pl_buf.len);
    copy[this->live_pl_buf.len] = 0;
    int n = this->rewritePlaylistBody(copy, out, kOutCap);
    free(copy);
    if (n > 0) {
      mkdir("/tmp/padwave-src", 0755);
      FILE *f = fopen("/tmp/padwave-src/index.m3u8", "w");
      if (f) {
        fwrite(out, 1, static_cast<size_t>(n), f);
        fclose(f);
      }
    }
    free(out);
  }

  bool decodeLiveSegment(const char *file_path) {
    if (!file_path || !this->sink_playback || !this->player.sinkMode()) return false;
    if (strcmp(file_path, this->live_decoded) == 0) return true;

    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec  = nullptr;
    SwrContext *swr      = nullptr;
    AVPacket *pkt        = nullptr;
    AVFrame *frame       = nullptr;
    int audio_index      = -1;
    bool ok              = false;

    if (avformat_open_input(&fmt, file_path, nullptr, nullptr) < 0) goto done;
    if (avformat_find_stream_info(fmt, nullptr) < 0) goto done;
    this->live_has_video =
        av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) >= 0;
    audio_index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_index < 0) goto done;
    {
      const AVCodec *codec = avcodec_find_decoder(fmt->streams[audio_index]->codecpar->codec_id);
      if (!codec) goto done;
      dec = avcodec_alloc_context3(codec);
      if (!dec ||
          avcodec_parameters_to_context(dec, fmt->streams[audio_index]->codecpar) < 0 ||
          avcodec_open2(dec, codec, nullptr) < 0)
        goto done;
    }
    {
      AVChannelLayout in_layout;
      AVChannelLayout out_layout;
      av_channel_layout_copy(&in_layout, &dec->ch_layout);
      av_channel_layout_default(&out_layout, CHANNELS);
      int rc = swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, RATE, &in_layout,
                                   dec->sample_fmt, dec->sample_rate, 0, nullptr);
      av_channel_layout_uninit(&in_layout);
      av_channel_layout_uninit(&out_layout);
      if (rc < 0 || !swr || swr_init(swr) < 0) goto done;
    }
    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) goto done;

    {
      float out[8192 * CHANNELS];
      auto drain = [&]() {
        for (;;) {
          int ret = avcodec_receive_frame(dec, frame);
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
          if (ret < 0) break;
          int cap = swr_get_out_samples(swr, frame->nb_samples);
          if (cap > 8192) cap = 8192;
          uint8_t *dst[1] = {reinterpret_cast<uint8_t *>(out)};
          const uint8_t *src[AV_NUM_DATA_POINTERS] = {};
          int channels = dec->ch_layout.nb_channels;
          if (channels > AV_NUM_DATA_POINTERS) channels = AV_NUM_DATA_POINTERS;
          for (int i = 0; i < channels; i++) src[i] = frame->extended_data[i];
          int got = swr_convert(swr, dst, cap, src, frame->nb_samples);
          if (got > 0) this->appendPcm(out, static_cast<size_t>(got));
          av_frame_unref(frame);
        }
      };

      while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == audio_index) avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        drain();
      }
      avcodec_send_packet(dec, nullptr);
      drain();
    }
    ok = true;

done:
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (swr) swr_free(&swr);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    if (ok) {
      this->live_decode_ok++;
      snprintf(this->live_decoded, sizeof(this->live_decoded), "%s", file_path);
    } else {
      this->live_decode_fail++;
    }
    return ok;
  }

  bool fetchLiveSegment(const char *url, char *file_path, size_t cap) {
    const char *dir = "/tmp/padwave-src";
    mkdir(dir, 0755);
    char hash[17];
    makeId(url, hash);
    snprintf(file_path, cap, "%s/%s.ts", dir, hash);
    if (access(file_path, R_OK) == 0) return true;
    return fetchUrlToFile(url, file_path);
  }

  bool liveSourceSegment(const char *encoded, char *file_path, size_t cap) {
    if (!encoded || !*encoded || !this->live_active) return false;
    char url[kMaxPath * 2];
    if (!urlDecode(encoded, url, sizeof(url)) || !validLiveUrl(url)) return false;
    return fetchLiveSegment(url, file_path, cap);
  }

  static void resolveRelative(const char *base, const char *ref, char *out, size_t cap) {
    if (strncasecmp(ref, "http://", 7) == 0 || strncasecmp(ref, "https://", 8) == 0) {
      snprintf(out, cap, "%s", ref);
      return;
    }
    const char *q     = strchr(base, '?');
    size_t base_len   = q ? static_cast<size_t>(q - base) : strlen(base);
    const char *slash = nullptr;
    for (const char *p = base; p < base + base_len; p++)
      if (*p == '/') slash = p;
    size_t dir_len = slash ? static_cast<size_t>(slash - base) + 1 : 0;
    if (dir_len >= cap) dir_len = cap - 1;
    memcpy(out, base, dir_len);
    out[dir_len] = 0;
    snprintf(out + dir_len, cap - dir_len, "%s", ref);
  }

  void resolveLiveStream(const char *channel_url, char *out, size_t cap) {
    snprintf(out, cap, "%s", channel_url);
    constexpr size_t kPlaylistMax = 2 * 1024 * 1024;
    char *body                   = static_cast<char *>(malloc(kPlaylistMax));
    if (!body) return;
    int n = fetchUrl(channel_url, body, kPlaylistMax);
    if (n <= 0) {
      free(body);
      return;
    }
    char *p = strstr(body, "#EXT-X-STREAM-INF");
    if (p) {
      p = strchr(p, '\n');
      if (p) p++;
      while (p && *p) {
        char *eol = strpbrk(p, "\r\n");
        char *end = eol ? eol : p + strlen(p);
        char save = *end;
        *end      = 0;
        char *uri = trimLine(p);
        if (*uri && *uri != '#') {
          resolveRelative(channel_url, uri, out, cap);
          *end = save;
          break;
        }
        *end = save;
        if (!eol) break;
        p = eol + 1;
        while (*p == '\r' || *p == '\n') p++;
      }
    }
    free(body);
  }

  void stopLive(bool preserve_mode = false) {
    this->resetLiveCurl();
    this->resetLiveRaw();
    this->live_raw         = false;
    this->live_active      = false;
    this->live_current     = -1;
    this->live_audio_synced = false;
    this->live_audio_seq   = 0;
    this->live_audio_at    = 0;
    this->live_decoded[0]  = 0;
    this->live_has_video   = false;
    this->live_pcm_frames  = 0;
    this->live_pcm_pos     = 0;
    this->live_error[0]    = 0;
    if (!preserve_mode) {
      snprintf(this->playback_mode, sizeof(this->playback_mode), "idle");
      this->stream_url[0] = 0;
    }
    this->sinkPlaybackStop();
    if (!preserve_mode) configSave();
  }

  static bool isHlsUrl(const char *url) {
    const char *q = strchr(url, '?');
    size_t len = q ? static_cast<size_t>(q - url) : strlen(url);
    return (len >= 5 && strncasecmp(url + len - 5, ".m3u8", 5) == 0) ||
           (len >= 4 && strncasecmp(url + len - 4, ".m3u", 4) == 0);
  }

  bool startLiveSource(const char *channel_url) {
    if (!channel_url || !validLiveUrl(channel_url)) return false;
    system("rm -rf /tmp/padwave-src");
    mkdir("/tmp/padwave-src", 0755);
    snprintf(this->live_source_url, sizeof(this->live_source_url), "%s", channel_url);
    this->live_raw = !isHlsUrl(channel_url);
    if (this->live_raw) {
      snprintf(this->live_upstream_url, sizeof(this->live_upstream_url), "%s", channel_url);
    } else {
      char upstream[kMaxPath];
      resolveLiveStream(channel_url, upstream, sizeof(upstream));
      snprintf(this->live_upstream_url, sizeof(this->live_upstream_url), "%s", upstream);
    }
    this->live_decoded[0]   = 0;
    this->live_audio_synced = false;
    this->live_audio_seq    = 0;
    this->live_audio_at     = 0;
    this->live_next_ready   = false;
    this->live_next_url[0]  = 0;
    this->resetLiveRaw();
    this->live_has_video    = false;
    this->live_pcm_frames   = 0;
    this->live_pcm_pos      = 0;
    this->live_error[0]     = 0;
    this->live_active       = true;
    snprintf(this->playback_mode, sizeof(this->playback_mode), "stream");
    snprintf(this->stream_url, sizeof(this->stream_url), "%s", channel_url);
    configSave();
    return true;
  }

  static size_t curlBufferWrite(char *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuffer *b = static_cast<CurlBuffer *>(userdata);
    size_t n      = size * nmemb;
    if (b->len + n >= b->cap) {
      b->overflow = true;
      return 0;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
  }

  void configureLiveCurl(CURL *h, const char *url, CurlBuffer *buffer, long max_bytes, bool use_proxy) const {
    buffer->len      = 0;
    buffer->overflow = false;
    if (buffer->cap) buffer->data[0] = 0;
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 20000L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "padwave/1");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curlBufferWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, buffer);
    curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(max_bytes));
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    this->applyCurlProxy(h, use_proxy);
  }

  void resetLiveCurl() {
    if (!this->live_multi) return;
    if (this->live_pl_curl) {
      curl_multi_remove_handle(this->live_multi, this->live_pl_curl);
      curl_easy_cleanup(this->live_pl_curl);
      this->live_pl_curl = nullptr;
    }
    if (this->live_seg_curl) {
      curl_multi_remove_handle(this->live_multi, this->live_seg_curl);
      curl_easy_cleanup(this->live_seg_curl);
      this->live_seg_curl = nullptr;
    }
    this->live_pl_inflight  = false;
    this->live_seg_inflight = false;
    this->live_pl_proxied   = false;
    this->live_seg_proxied  = false;
    this->live_next_ready   = false;
  }

  static size_t curlRawWrite(char *ptr, size_t size, size_t nmemb, void *userdata) {
    App *app = static_cast<App *>(userdata);
    size_t n  = size * nmemb;
    constexpr size_t cap = sizeof(app->live_raw_ring);
    if (app->live_raw_head + n > cap && app->live_raw_tail > 0) {
      size_t used = app->live_raw_head - app->live_raw_tail;
      memmove(app->live_raw_ring, app->live_raw_ring + app->live_raw_tail, used);
      app->live_raw_head = used;
      app->live_raw_tail = 0;
    }
    size_t room = cap - app->live_raw_head;
    size_t copy = n < room ? n : room;
    if (copy > 0) {
      memcpy(app->live_raw_ring + app->live_raw_head, ptr, copy);
      app->live_raw_head += copy;
    }
    return n;
  }

  void configureRawCurl(CURL *h, bool use_proxy) {
    curl_easy_setopt(h, CURLOPT_URL, this->live_upstream_url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "padwave/1");
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 32768L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curlRawWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    this->applyCurlProxy(h, use_proxy);
  }

  bool startRawFetch(bool use_proxy = false) {
    if (!this->live_multi || !this->live_active || !this->live_raw || this->live_raw_inflight)
      return false;
    CURL *h = curl_easy_init();
    if (!h) return false;
    this->configureRawCurl(h, use_proxy);
    this->live_raw_headers = curl_slist_append(this->live_raw_headers, "Icy-MetaData: 0");
    this->live_raw_headers = curl_slist_append(this->live_raw_headers, "Accept: audio/*");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, this->live_raw_headers);
    if (curl_multi_add_handle(this->live_multi, h) != CURLM_OK) {
      curl_easy_cleanup(h);
      return false;
    }
    this->live_raw_curl     = h;
    this->live_raw_inflight = true;
    this->live_raw_proxied  = use_proxy;
    return true;
  }

  bool initRawDecoder(int codec_id) {
    const AVCodec *codec = avcodec_find_decoder(static_cast<AVCodecID>(codec_id));
    if (!codec) return false;
    this->live_raw_parser = av_parser_init(codec_id);
    this->live_raw_dec    = avcodec_alloc_context3(codec);
    this->live_raw_frame  = av_frame_alloc();
    if (!this->live_raw_parser || !this->live_raw_dec || !this->live_raw_frame) return false;
    if (avcodec_open2(this->live_raw_dec, codec, nullptr) < 0) return false;
    this->live_raw_codec = codec_id;
    return true;
  }

  void decodeRawAudio() {
    if (!this->live_raw || !this->live_active || !this->sink_playback) return;
    size_t avail = this->live_raw_head - this->live_raw_tail;
    if (avail == 0) return;
    if (this->live_raw_codec == -3) {
      this->decodeOggAudio();
      return;
    }
    if (this->live_raw_codec < 0) {
      size_t scan = avail < 4096 ? avail : 4096;
      const uint8_t *p = this->live_raw_ring + this->live_raw_tail;
      if (scan >= 4 && p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S') {
        this->live_raw_codec = -3;
        this->decodeOggAudio();
        return;
      }
      int codec_id = -1;
      for (size_t i = 0; i + 1 < scan; i++) {
        if (p[i] == 'I' && i + 2 < scan && p[i + 1] == 'D' && p[i + 2] == '3') {
          codec_id = AV_CODEC_ID_MP3;
          break;
        }
        if (p[i] == 0xFF && (p[i + 1] & 0xE0) == 0xE0) {
          codec_id = (p[i + 1] & 0xF6) == 0xF0 ? AV_CODEC_ID_AAC : AV_CODEC_ID_MP3;
          break;
        }
      }
      if (codec_id < 0) {
        if (scan >= 4096) this->live_decode_fail++;
        return;
      }
      if (!this->initRawDecoder(codec_id)) {
        this->live_decode_fail++;
        this->live_raw_codec = -2;
        return;
      }
    }
    if (this->live_raw_codec < 0) return;

    AVPacket *pkt = nullptr;
    for (int guard = 0; guard < 64; guard++) {
      avail = this->live_raw_head - this->live_raw_tail;
      if (avail == 0) break;
      uint8_t *out = nullptr;
      int out_size = 0;
      int used = av_parser_parse2(this->live_raw_parser, this->live_raw_dec, &out, &out_size,
                                  this->live_raw_ring + this->live_raw_tail,
                                  static_cast<int>(avail), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (used < 0) {
        this->live_raw_tail++;
        continue;
      }
      if (used == 0) break;
      this->live_raw_tail += static_cast<size_t>(used);
      if (out_size <= 0) continue;
      pkt = av_packet_alloc();
      if (!pkt) break;
      if (av_new_packet(pkt, out_size) < 0) {
        av_packet_free(&pkt);
        break;
      }
      memcpy(pkt->data, out, static_cast<size_t>(out_size));
      int send_rc = avcodec_send_packet(this->live_raw_dec, pkt);
      av_packet_free(&pkt);
      pkt = nullptr;
      if (send_rc < 0 && send_rc != AVERROR(EAGAIN)) continue;
      while (avcodec_receive_frame(this->live_raw_dec, this->live_raw_frame) == 0) {
        if (!this->live_raw_swr) {
          AVChannelLayout in_layout;
          AVChannelLayout out_layout;
          av_channel_layout_copy(&in_layout, &this->live_raw_frame->ch_layout);
          av_channel_layout_default(&out_layout, CHANNELS);
          int rc = swr_alloc_set_opts2(&this->live_raw_swr, &out_layout, AV_SAMPLE_FMT_FLT, RATE,
                                       &in_layout, this->live_raw_dec->sample_fmt,
                                       this->live_raw_dec->sample_rate, 0, nullptr);
          av_channel_layout_uninit(&in_layout);
          av_channel_layout_uninit(&out_layout);
          if (rc < 0 || !this->live_raw_swr || swr_init(this->live_raw_swr) < 0) break;
        }
        float out_pcm[8192 * CHANNELS];
        int cap = swr_get_out_samples(this->live_raw_swr, this->live_raw_frame->nb_samples);
        if (cap > 8192) cap = 8192;
        uint8_t *dst[1] = {reinterpret_cast<uint8_t *>(out_pcm)};
        const uint8_t *src[AV_NUM_DATA_POINTERS] = {};
        int channels = this->live_raw_dec->ch_layout.nb_channels;
        if (channels > AV_NUM_DATA_POINTERS) channels = AV_NUM_DATA_POINTERS;
        for (int i = 0; i < channels; i++) src[i] = this->live_raw_frame->extended_data[i];
        int got = swr_convert(this->live_raw_swr, dst, cap, src,
                              this->live_raw_frame->nb_samples);
        if (got > 0) this->appendPcm(out_pcm, static_cast<size_t>(got));
        av_frame_unref(this->live_raw_frame);
      }
      if (this->live_pcm_frames + RATE / 10 >= kLivePcmSeconds * RATE) break;
    }
  }

  void decodeOpusFrames() {
    if (!this->live_raw_dec || !this->live_raw_frame) return;
    while (avcodec_receive_frame(this->live_raw_dec, this->live_raw_frame) == 0) {
      if (!this->live_raw_swr) {
        AVChannelLayout in_layout;
        AVChannelLayout out_layout;
        av_channel_layout_copy(&in_layout, &this->live_raw_frame->ch_layout);
        av_channel_layout_default(&out_layout, CHANNELS);
        int rc = swr_alloc_set_opts2(&this->live_raw_swr, &out_layout, AV_SAMPLE_FMT_FLT, RATE,
                                     &in_layout, this->live_raw_dec->sample_fmt,
                                     this->live_raw_dec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (rc < 0 || !this->live_raw_swr || swr_init(this->live_raw_swr) < 0) break;
      }
      float out_pcm[8192 * CHANNELS];
      int cap = swr_get_out_samples(this->live_raw_swr, this->live_raw_frame->nb_samples);
      if (cap > 8192) cap = 8192;
      uint8_t *dst[1] = {reinterpret_cast<uint8_t *>(out_pcm)};
      const uint8_t *src[AV_NUM_DATA_POINTERS] = {};
      int channels = this->live_raw_dec->ch_layout.nb_channels;
      if (channels > AV_NUM_DATA_POINTERS) channels = AV_NUM_DATA_POINTERS;
      for (int i = 0; i < channels; i++) src[i] = this->live_raw_frame->extended_data[i];
      int got = swr_convert(this->live_raw_swr, dst, cap, src,
                            this->live_raw_frame->nb_samples);
      if (got > 0) this->appendPcm(out_pcm, static_cast<size_t>(got));
      av_frame_unref(this->live_raw_frame);
    }
  }

  bool initOggOpus(const uint8_t *head, size_t len) {
    if (len < 19 || memcmp(head, "OpusHead", 8) != 0) return false;
    int channels = head[9];
    if (channels < 1) channels = 2;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) return false;
    if (this->live_raw_frame) av_frame_free(&this->live_raw_frame);
    if (this->live_raw_swr) swr_free(&this->live_raw_swr);
    if (this->live_raw_dec) avcodec_free_context(&this->live_raw_dec);
    this->live_raw_swr = nullptr;
    this->live_raw_dec = avcodec_alloc_context3(codec);
    this->live_raw_frame = av_frame_alloc();
    if (!this->live_raw_dec || !this->live_raw_frame) return false;
    this->live_raw_dec->sample_rate = 48000;
    av_channel_layout_default(&this->live_raw_dec->ch_layout, channels);
    this->live_raw_dec->extradata =
        static_cast<uint8_t *>(av_mallocz(len + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!this->live_raw_dec->extradata) return false;
    memcpy(this->live_raw_dec->extradata, head, len);
    this->live_raw_dec->extradata_size = static_cast<int>(len);
    if (avcodec_open2(this->live_raw_dec, codec, nullptr) < 0) return false;
    this->live_raw_codec = -3;
    return true;
  }

  void decodeOggPacket(const uint8_t *data, size_t len) {
    if (!this->live_raw_dec || len == 0) return;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return;
    if (av_new_packet(pkt, static_cast<int>(len)) < 0) {
      av_packet_free(&pkt);
      return;
    }
    memcpy(pkt->data, data, len);
    int rc = avcodec_send_packet(this->live_raw_dec, pkt);
    av_packet_free(&pkt);
    if (rc >= 0 || rc == AVERROR(EAGAIN)) this->decodeOpusFrames();
  }

  void handleOggPacket(uint32_t serial) {
    const uint8_t *data = this->live_ogg_packet;
    size_t len = this->live_ogg_packet_len;
    if (len == 0) return;
    if (this->live_ogg_serial < 0) {
      if (len >= 8 && memcmp(data, "OpusHead", 8) == 0) {
        if (this->initOggOpus(data, len))
          this->live_ogg_serial = static_cast<int>(serial);
      }
      return;
    }
    if (static_cast<int>(serial) != this->live_ogg_serial) return;
    if (len >= 8 && memcmp(data, "OpusHead", 8) == 0) {
      this->initOggOpus(data, len);
      return;
    }
    if (len >= 8 && memcmp(data, "OpusTags", 8) == 0) return;
    this->decodeOggPacket(data, len);
  }

  void decodeOggAudio() {
    for (;;) {
      size_t avail = this->live_raw_head - this->live_raw_tail;
      if (avail < 27) return;
      uint8_t *p = this->live_raw_ring + this->live_raw_tail;
      if (memcmp(p, "OggS", 4) != 0) {
        this->live_raw_tail++;
        continue;
      }
      size_t nseg = p[26];
      size_t hdr  = 27 + nseg;
      if (avail < hdr) return;
      size_t body = 0;
      for (size_t i = 0; i < nseg; i++) body += p[27 + i];
      if (avail < hdr + body) return;
      uint8_t flags = p[5];
      uint32_t serial = static_cast<uint32_t>(p[14]) |
                        (static_cast<uint32_t>(p[15]) << 8) |
                        (static_cast<uint32_t>(p[16]) << 16) |
                        (static_cast<uint32_t>(p[17]) << 24);
      bool use = this->live_ogg_serial < 0 ||
                 static_cast<int>(serial) == this->live_ogg_serial;
      if (use) {
        if (!(flags & 0x01)) this->live_ogg_packet_len = 0;
        const uint8_t *b = p + hdr;
        for (size_t i = 0; i < nseg; i++) {
          size_t n = p[27 + i];
          if (this->live_ogg_packet_len + n <= sizeof(this->live_ogg_packet)) {
            memcpy(this->live_ogg_packet + this->live_ogg_packet_len, b, n);
            this->live_ogg_packet_len += n;
          } else {
            this->live_ogg_packet_len = 0;
          }
          b += n;
          if (n < 255) {
            this->handleOggPacket(serial);
            this->live_ogg_packet_len = 0;
          }
        }
      }
      this->live_raw_tail += hdr + body;
    }
  }

  void resetRawDecoder() {
    if (this->live_raw_frame) av_frame_free(&this->live_raw_frame);
    if (this->live_raw_swr) swr_free(&this->live_raw_swr);
    if (this->live_raw_dec) avcodec_free_context(&this->live_raw_dec);
    if (this->live_raw_parser) av_parser_close(this->live_raw_parser);
    this->live_raw_frame  = nullptr;
    this->live_raw_swr    = nullptr;
    this->live_raw_dec    = nullptr;
    this->live_raw_parser = nullptr;
    this->live_raw_codec  = -1;
    this->live_ogg_serial = -1;
    this->live_ogg_packet_len = 0;
    this->live_raw_head   = 0;
    this->live_raw_tail   = 0;
  }

  void resetLiveRaw() {
    if (this->live_multi && this->live_raw_curl) {
      curl_multi_remove_handle(this->live_multi, this->live_raw_curl);
      curl_easy_cleanup(this->live_raw_curl);
    }
    this->live_raw_curl     = nullptr;
    this->live_raw_inflight = false;
    this->live_raw_proxied  = false;
    if (this->live_raw_headers) {
      curl_slist_free_all(this->live_raw_headers);
      this->live_raw_headers = nullptr;
    }
    this->resetRawDecoder();
  }

  void appendPcm(const float *data, size_t frames) {
    size_t room = (kLivePcmSeconds * RATE) - this->live_pcm_frames;
    if (frames > room) frames = room;
    if (frames == 0) return;
    memcpy(this->live_pcm + this->live_pcm_frames * CHANNELS, data,
           frames * CHANNELS * sizeof(float));
    this->live_pcm_frames += frames;
  }

  bool startLivePlaylistFetch(bool use_proxy = false) {
    if (!this->live_multi || !this->live_active || this->live_pl_inflight) return false;
    CURL *h = curl_easy_init();
    if (!h) return false;
    this->configureLiveCurl(h, this->live_upstream_url, &this->live_pl_buf, kPlaylistBytes, use_proxy);
    if (curl_multi_add_handle(this->live_multi, h) != CURLM_OK) {
      curl_easy_cleanup(h);
      return false;
    }
    this->live_pl_curl     = h;
    this->live_pl_inflight = true;
    this->live_pl_proxied  = use_proxy;
    return true;
  }

  bool startLiveSegmentFetch(bool use_proxy = false) {
    if (!this->live_multi || !this->live_active || this->live_seg_inflight || !this->live_next_url[0])
      return false;
    CURL *h = curl_easy_init();
    if (!h) return false;
    this->configureLiveCurl(h, this->live_next_url, &this->live_seg_buf, kSegmentBytes, use_proxy);
    if (curl_multi_add_handle(this->live_multi, h) != CURLM_OK) {
      curl_easy_cleanup(h);
      return false;
    }
    this->live_seg_curl     = h;
    this->live_seg_inflight = true;
    this->live_seg_proxied  = use_proxy;
    return true;
  }

  void parseLivePlaylist() {
    this->live_next_ready = false;
    this->live_next_url[0] = 0;
    char *body = this->live_pl_buf.data;
    if (!body) return;
    long base = 0;
    long index = 0;
    double duration = 0.0;
    long chosen_seq = -1;
    double chosen_duration = 0.0;
    char chosen[kMaxPath] = "";
    char *save = nullptr;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
      line = trimLine(line);
      if (!*line) continue;
      if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
        base = strtol(line + 22, nullptr, 10);
        continue;
      }
      if (strncmp(line, "#EXTINF:", 8) == 0) {
        duration = atof(line + 8);
        continue;
      }
      if (*line == '#') continue;
      char absolute[kMaxPath * 2];
      resolveRelative(this->live_upstream_url, line, absolute, sizeof(absolute));
      long seq = base + index++;
      if (!this->live_audio_synced) {
        chosen_seq = seq;
        chosen_duration = duration;
        snprintf(chosen, sizeof(chosen), "%s", absolute);
      } else if (seq > this->live_audio_seq && chosen_seq < 0) {
        chosen_seq = seq;
        chosen_duration = duration;
        snprintf(chosen, sizeof(chosen), "%s", absolute);
      }
    }
    if (chosen_seq < 0) {
      this->live_audio_at = nowMs() + 1000;
      return;
    }
    this->live_next_seq  = chosen_seq;
    this->live_next_dur  = chosen_duration;
    snprintf(this->live_next_url, sizeof(this->live_next_url), "%s", chosen);
    this->live_next_ready = true;
  }

  void compactLivePcm() {
    if (this->live_pcm_pos == 0) return;
    size_t remaining = this->live_pcm_frames - this->live_pcm_pos;
    if (remaining > 0)
      memmove(this->live_pcm, this->live_pcm + this->live_pcm_pos * CHANNELS,
              remaining * CHANNELS * sizeof(float));
    this->live_pcm_frames = remaining;
    this->live_pcm_pos    = 0;
  }

  void serviceLiveFeed() {
    if (!this->live_active || !this->sink_playback || !this->player.sinkMode()) return;
    if (this->live_pcm_pos > 0) this->compactLivePcm();
    while (this->live_pcm_frames > 0) {
      int free_blocks = this->player.sinkFreeBlocks();
      if (free_blocks <= 0) break;
      size_t available = static_cast<size_t>(free_blocks) * IN_FRAMES;
      size_t frames    = this->live_pcm_frames < available ? this->live_pcm_frames : available;
      this->player.pushSink(this->live_pcm, frames);
      this->live_pcm_pos    += frames;
      this->live_pcm_frames -= frames;
      if (this->live_pcm_frames > 0) {
        memmove(this->live_pcm, this->live_pcm + this->live_pcm_pos * CHANNELS,
                this->live_pcm_frames * CHANNELS * sizeof(float));
        this->live_pcm_pos = 0;
      } else {
        this->live_pcm_pos = 0;
      }
    }
  }

  void serviceLiveMulti() {
    if (!this->live_multi || !this->live_active || !this->sink_playback ||
        !this->player.sinkMode())
      return;
    int running = 0;
    curl_multi_perform(this->live_multi, &running);
    CURLMsg *msg = nullptr;
    int left     = 0;
    while ((msg = curl_multi_info_read(this->live_multi, &left)) != nullptr) {
      if (msg->msg != CURLMSG_DONE) continue;
      CURL *h = msg->easy_handle;
      long code = 0;
      curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
      bool ok = msg->data.result == CURLE_OK && code >= 200 && code < 300;
      if (h == this->live_pl_curl) {
        curl_multi_remove_handle(this->live_multi, h);
        curl_easy_cleanup(h);
        this->live_pl_curl     = nullptr;
        this->live_pl_inflight = false;
        if (ok && !this->live_pl_buf.overflow) {
          this->writeLivePlaylistFile();
          this->parseLivePlaylist();
        } else if (!this->live_pl_proxied && this->live_proxy[0]) {
          this->startLivePlaylistFetch(true);
        } else {
          this->live_audio_at = nowMs() + 1000;
        }
      } else if (h == this->live_raw_curl) {
        curl_multi_remove_handle(this->live_multi, h);
        curl_easy_cleanup(h);
        this->live_raw_curl     = nullptr;
        this->live_raw_inflight = false;
        if (this->live_raw_headers) {
          curl_slist_free_all(this->live_raw_headers);
          this->live_raw_headers = nullptr;
        }
        this->decodeRawAudio();
        this->resetRawDecoder();
        if (!ok && !this->live_raw_proxied && this->live_proxy[0]) {
          this->startRawFetch(true);
        } else {
          this->live_audio_at = nowMs() + 2000;
        }
      } else if (h == this->live_seg_curl) {
        curl_multi_remove_handle(this->live_multi, h);
        curl_easy_cleanup(h);
        this->live_seg_curl     = nullptr;
        this->live_seg_inflight = false;
        if (ok && !this->live_seg_buf.overflow && this->live_next_url[0]) {
          const char *dir = "/tmp/padwave-src";
          mkdir(dir, 0755);
          char hash[17];
          makeId(this->live_next_url, hash);
          char file_path[512];
          snprintf(file_path, sizeof(file_path), "%s/%s.ts", dir, hash);
          FILE *f = fopen(file_path, "wb");
          bool wrote = false;
          if (f) {
            wrote = fwrite(this->live_seg_buf.data, 1, this->live_seg_buf.len, f) ==
                    this->live_seg_buf.len;
            fclose(f);
          }
          if (wrote && this->decodeLiveSegment(file_path)) {
            this->live_audio_synced = true;
            this->live_audio_seq    = this->live_next_seq;
            this->live_next_ready   = false;
            this->live_audio_at     = nowMs() + 500;
          } else {
            this->live_audio_at = nowMs() + 1000;
          }
        } else if (!this->live_seg_proxied && this->live_proxy[0]) {
          this->startLiveSegmentFetch(true);
        } else {
          this->live_audio_at = nowMs() + 1000;
        }
      }
    }

    if (this->live_raw) {
      this->decodeRawAudio();
      if (!this->live_raw_inflight && nowMs() >= this->live_audio_at)
        this->startRawFetch();
      return;
    }

    if (this->live_pl_inflight || this->live_seg_inflight) return;
    if (this->live_next_ready) {
      this->compactLivePcm();
      size_t needed = static_cast<size_t>(this->live_next_dur * RATE);
      if (needed == 0) needed = 1;
      if (this->live_pcm_frames + needed <= kLivePcmSeconds * RATE) this->startLiveSegmentFetch();
      return;
    }
    if (nowMs() >= this->live_audio_at) this->startLivePlaylistFetch();
  }

  int findLiveUrl(const char *url) const {
    for (int i = 0; i < this->live_count; i++)
      if (strcmp(this->live_channels[i].url, url) == 0) return i;
    return -1;
  }

  bool startLive(int index) {
    if (index < 0 || index >= this->live_count)
      return false;
    stopLive();
    if (!this->sinkPlaybackStart()) {
      snprintf(this->live_error, sizeof(this->live_error), "controller/player is not ready");
      return false;
    }
    if (!this->startLiveSource(this->live_channels[index].url)) {
      this->sinkPlaybackStop();
      return false;
    }
    this->live_current = index;
    return true;
  }

  bool startLiveDirect(const char *url) {
    if (!validLiveUrl(url)) return false;
    stopLive();
    if (!this->sinkPlaybackStart()) {
      snprintf(this->live_error, sizeof(this->live_error), "controller/player is not ready");
      return false;
    }
    if (!this->startLiveSource(url)) {
      this->sinkPlaybackStop();
      return false;
    }
    this->live_current = -1;
    return true;
  }

  void scanDir(const char *dir, const char *folder) {
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
        char child_folder[kMaxFolder];
        if (folder && *folder)
          snprintf(child_folder, sizeof(child_folder), "%s/%s", folder, e->d_name);
        else
          snprintf(child_folder, sizeof(child_folder), "%s", e->d_name);
        scanDir(path, child_folder);
      } else if (S_ISREG(st.st_mode) && audioExt(e->d_name)) {
        addTrack(path, folder, st);
      }
    }
    closedir(d);
  }

  int findTrack(const char *path) {
    for (int i = 0; i < this->count; i++)
      if (strcmp(this->tracks[i].path, path) == 0) return i;
    return -1;
  }

  int findId(const char *id) const {
    for (int i = 0; i < this->count; i++)
      if (strcmp(this->tracks[i].id, id) == 0) return i;
    return -1;
  }

  void pushHistory(int index) {
    if (index < 0) return;
    if (this->history_len < kMaxHistory) {
      this->history[this->history_len++] = index;
    } else {
      memmove(this->history, this->history + 1, sizeof(int) * (kMaxHistory - 1));
      this->history[kMaxHistory - 1] = index;
    }
  }

  void playerSend(const char *line) {
    if (this->player) this->player.command(line);
  }

  void playerStart() {
    if (this->player) return;
    if (!this->player.create()) return;
    this->player.attach(this);
    this->player.setPcmTap(App::pcmTap, this);
    this->player_timer_fd = this->player.timerFd();
    applyOutputRoute();
    char line[32];
    snprintf(line, sizeof(line), "volume %d\n", this->volume);
    playerSend(line);
    snprintf(line, sizeof(line), "speed %d\n", this->speed);
    playerSend(line);
    snprintf(line, sizeof(line), "haptic %d %d %d %d\n", this->haptic_subwoofer ? 1 : 0,
             this->haptic_cutoff_hz, this->haptic_gain, this->haptics_only ? 1 : 0);
    playerSend(line);
    if (this->paused) playerSend("pause 1\n");
    this->led_force = true;
    serviceLeds();
  }

  void inputStart() {
    if (this->input.fd() >= 0) return;
    if (!this->player) return;
    debugLog("inputStart begin pfd=%d node=%s", this->player.fd(), this->player.node());
    if (this->player.fd() < 0 && !this->player.reopenDevice()) {
      debugLog("inputStart reopen failed");
      this->reconnect_at = static_cast<int>((nowMs() + 1500));
      return;
    }
    if (!this->input.open(this->player.fd(), this->player.node(), this->player.mac())) {
      debugLog("inputStart input.open failed fd=%d", this->player.fd());
      this->reconnect_at = static_cast<int>((nowMs() + 1500));
      return;
    }
    debugLog("inputStart ok pfd=%d node=%s mac=%s", this->player.fd(), this->player.node(),
             this->player.mac());
    if (this->player.mac()[0] &&
        strcmp(this->saved_controller_mac, this->player.mac()) != 0) {
      snprintf(this->saved_controller_mac, sizeof(this->saved_controller_mac), "%s",
               this->player.mac());
      configSave();
    }
    snprintf(this->input_node, sizeof(this->input_node), "%s", this->input.node());
    snprintf(this->controller_mac, sizeof(this->controller_mac), "%s", this->input.mac());
    this->input.attach(this);
    // A newly connected controller counts as a resume request: never leave it
    // connected to a zero-volume paused session.
    ensureAudibleVolume();
    this->led_force = true;
    serviceLeds();
  }

  void inputStop(bool pause_playback = false) {
    (void)pause_playback;
    debugLog("inputStop live=%d sink=%d playing=%d paused=%d web=%d pfd=%d", this->live_active,
             this->sink_playback, this->playing, this->paused, this->web_audio_clients,
             this->player.fd());
    if (this->current >= 0 && !this->paused) {
      debugLog("inputStop pausing once current=%d", this->current);
      this->paused = true;
      if (this->player) playerSend("pause 1\n");
      this->playhead_dirty = true;
    }
    if (this->live_active) stopLive();
    this->input.close();
    if (this->player) this->player.dropDevice();
    this->controller = false;
    this->input_node[0]     = 0;
    this->controller_mac[0] = 0;
    resetLinkStats();
    this->plugged_usb_data    = false;
    this->pluggedUsbPower     = false;
    this->controller_disabled = false;
    this->rate                = 0;
    this->rate_at             = 0;
    this->rate_frames         = 0;
    this->led_force           = true;
    serviceLeds();
  }

  void playerStatus(const struct player_status &st) {
    pos_ms    = st.pos_ms;
    dur_ms    = st.dur_ms;
    sent      = st.sent;
    underruns = st.underruns;
    paused    = st.paused != 0;
    eof       = st.eof != 0;
  }

  void playerTrack(const char *path) {
    int idx = findTrack(path);
    if (idx < 0) return;
    if (current >= 0 && current != idx) pushHistory(current);
    current        = idx;
    paused         = false;
    playing        = true;
    playhead_dirty = true;
    queueTrack(nextTrackIndex(idx));
  }

  int nextTrackIndex(int from) const {
    if (this->count == 0) return -1;
    if (from < 0 || from >= this->count) return 0;
    return (from + 1) % this->count;
  }

  int previousTrackIndex(int from) const {
    if (this->count == 0) return -1;
    if (from < 0 || from >= this->count) return this->count - 1;
    return (from + this->count - 1) % this->count;
  }

  void queueTrack(int index) {
    if (index < 0 || index >= this->count || this->count <= 1) return;
    char line[kMaxPath + 16];
    snprintf(line, sizeof(line), "queue %s\n", this->tracks[index].path);
    playerSend(line);
  }

  bool externalHost() const { return this->plugged_usb_data && !this->player.isUsb(); }

  bool padwaveDisabled() const { return this->plugged_usb_data && this->controller_disabled; }

  static const char *audioPathName(dualsense::OutputBody::AudioOutputPath path) {
    using Path = dualsense::OutputBody::AudioOutputPath;
    if (path == Path::HeadphonesStereo) return "headphones-stereo";
    if (path == Path::HeadphonesMono) return "headphones-mono";
    if (path == Path::HeadphonesAndSpeaker) return "headphones-speaker";
    if (path == Path::SpeakerMono) return "speaker-mono";
    return "unknown";
  }

  void applyOutputRoute() {
    if (this->player) this->player.setSpeaker(!this->jack);
  }

  void play(int index, long start_ms) {
    if (index < 0 || index >= this->count) return;
    debugLog("play index=%d start=%ld controller=%d pfd=%d", index, start_ms, this->controller,
             this->player.fd());
    ensureAudibleVolume();
    playerStart();
    char line[kMaxPath + 64];
    snprintf(line, sizeof(line), "fade 1500\nplay %ld %s\n", start_ms, this->tracks[index].path);
    playerSend(line);

    if (this->current >= 0 && this->current != index && this->history_len == 0)
      pushHistory(this->current);
    snprintf(this->playback_mode, sizeof(this->playback_mode), "file");
    this->stream_url[0]  = 0;
    this->current        = index;
    this->playing        = true;
    this->paused         = false;
    this->eof            = false;
    this->pos_ms         = start_ms;
    this->dur_ms         = 0;
    this->idle_since     = nowMs();
    this->playhead_dirty = true;

    // Gapless: queue the next track in the sorted list for the junction.
    queueTrack(nextTrackIndex(index));
  }

  void next() {
    if (this->count == 0) return;
    play(nextTrackIndex(this->current), 0);
  }

  void nextRandom() {
    if (this->count == 0) return;
    int index = rand() % this->count;
    if (this->count > 1 && index == this->current) index = nextTrackIndex(index);
    play(index, 0);
  }

  void previous() {
    if (this->count == 0) return;
    play(previousTrackIndex(this->current), 0);
  }

  void shuffleFolder() {
    if (this->count == 0) return;
    const char *folder = this->current >= 0 ? this->tracks[this->current].folder : "";
    for (int tries = 0; tries < 100; tries++) {
      int i = rand() % this->count;
      if (strcmp(this->tracks[i].folder, folder) != 0) {
        play(i, 0);
        return;
      }
    }
    next();
  }

  void togglePause() {
    debugLog("togglePause playing=%d paused=%d playerPlaying=%d pfd=%d", this->playing,
             this->paused, this->player.isPlaying(), this->player.fd());
    if (!this->player || !this->player.isPlaying()) {
      ensureAudibleVolume();
      if (this->current >= 0) play(this->current, this->pos_ms);
      return;
    }
    if (this->paused) {
      ensureAudibleVolume();
      if (this->paused) {
        this->paused = false;
        playerSend("pause 0\n");
      }
      this->paused_by_volume     = false;
      this->paused_by_jack       = false;
      this->paused_by_disconnect = false;
      this->paused_by_mic        = false;
      this->playhead_dirty       = true;
    } else {
      this->paused               = true;
      this->paused_by_volume     = false;
      this->paused_by_disconnect = false;
      playerSend("pause 1\n");
      this->playhead_dirty = true;
    }
  }

  void setMicMonitor(bool on) {
    if (on == this->mic_monitor) return;
    this->mic_monitor = on;
    playerSend(on ? "mic 1\n" : "mic 0\n");
    if (!on) micReset();
    this->paused_by_mic = false;
  }

  void ensureAudibleVolume() {
    if (this->volume == 0)
      setVolume(this->last_nonzero_volume > 0 ? this->last_nonzero_volume : kDefaultVolume);
  }

  void setSpeed(int percent) {
    if (percent < 50) percent = 50;
    if (percent > 200) percent = 200;
    if (percent == this->speed) return;
    this->speed = percent;
    char line[32];
    snprintf(line, sizeof(line), "speed %d\n", percent);
    playerSend(line);
    configSave();
  }

  void setVolume(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v > 0) this->last_nonzero_volume = v;
    if (v != this->volume) {
      fprintf(stderr, "padwave: volume %d%s\n", v, v == 0 ? " (muted)" : "");
      this->volume_dirty    = true;
      this->volume_saved_at = nowMs();
    }
    this->volume = v;
    char line[32];
    snprintf(line, sizeof(line), "volume %d\n", v);
    playerSend(line);

    if (v == 0) {
      this->paused_by_disconnect = false;
      if (this->playing && !this->paused) {
        this->paused           = true;
        this->paused_by_volume = true;
        playerSend("pause 1\n");
        this->playhead_dirty = true;
      }
    } else if (this->paused_by_volume && this->playing) {
      this->paused           = false;
      this->paused_by_volume = false;
      playerSend("pause 0\n");
      this->playhead_dirty = true;
    }
  }

  void toggleMute() {
    if (this->volume == 0)
      setVolume(this->last_nonzero_volume > 0 ? this->last_nonzero_volume : kDefaultVolume);
    else
      setVolume(0);
  }

  void seek(long ms) {
    if (ms < 0) ms = 0;
    if (this->dur_ms > 0 && ms > this->dur_ms) ms = this->dur_ms;
    char line[32];
    snprintf(line, sizeof(line), "seek %ld\n", ms);
    playerSend(line);
    this->pos_ms         = ms;
    this->playhead_dirty = true;
  }

  static const char *dpadName(dualsense::InputBody::Dpad dpad) {
    if (dpad == dualsense::InputBody::Dpad::Up) return "up";
    if (dpad == dualsense::InputBody::Dpad::UpRight) return "up_right";
    if (dpad == dualsense::InputBody::Dpad::Right) return "right";
    if (dpad == dualsense::InputBody::Dpad::DownRight) return "down_right";
    if (dpad == dualsense::InputBody::Dpad::Down) return "down";
    if (dpad == dualsense::InputBody::Dpad::DownLeft) return "down_left";
    if (dpad == dualsense::InputBody::Dpad::Left) return "left";
    if (dpad == dualsense::InputBody::Dpad::UpLeft) return "up_left";
    return "none";
  }

  static void buttonsToString(const dualsense::InputBody::Buttons buttons, char *out, size_t cap) {
    struct {
      dualsense::InputBody::Button button;
      const char *name;
    } names[] = {
        {dualsense::InputBody::Button::ButtonSquare, "square"},
        {dualsense::InputBody::Button::ButtonCross, "cross"},
        {dualsense::InputBody::Button::ButtonCircle, "circle"},
        {dualsense::InputBody::Button::ButtonTriangle, "triangle"},
        {dualsense::InputBody::Button::L1, "l1"},
        {dualsense::InputBody::Button::R1, "r1"},
        {dualsense::InputBody::Button::L2, "l2"},
        {dualsense::InputBody::Button::R2, "r2"},
        {dualsense::InputBody::Button::Create, "create"},
        {dualsense::InputBody::Button::Options, "options"},
        {dualsense::InputBody::Button::L3, "l3"},
        {dualsense::InputBody::Button::R3, "r3"},
        {dualsense::InputBody::Button::PsHome, "ps"},
        {dualsense::InputBody::Button::Touchpad, "touchpad"},
        {dualsense::InputBody::Button::MicMute, "mute"},
        {dualsense::InputBody::Button::EdgeFn1, "fn1"},
        {dualsense::InputBody::Button::EdgeFn2, "fn2"},
        {dualsense::InputBody::Button::EdgeLeftPaddle, "left_paddle"},
        {dualsense::InputBody::Button::EdgeRightPaddle, "right_paddle"},
    };

    size_t o = 0;
    out[0]   = 0;
    for (auto &n : names) {
      if (!buttons.isPressed(n.button)) continue;
      int w = snprintf(out + o, cap - o, "%s%s", o ? "," : "", n.name);
      if (w <= 0 || static_cast<size_t>(w) >= cap - o) break;
      o += static_cast<size_t>(w);
    }
  }

  void inputFrame(const dualsense::InputBody &b, const uint8_t *raw_buf, size_t len) {
    bool was_connected = this->controller;
    this->controller   = true;
    if (!was_connected) {
      this->last_activity_ms = nowMs();
      this->idle_since       = this->last_activity_ms;
      if (this->power_save_mode) {
        this->power_save_mask         = kPowerSaveModeMask;
        this->power_save_leds_applied = false;
      } else {
        this->power_save_mask = 0;
      }
      this->led_force = true;
      this->serviceLeds();
    }
    const dualsense::InputBody::Stick left  = b.getLeftStick();
    const dualsense::InputBody::Stick right = b.getRightStick();
    this->lx                                = left.x;
    this->ly                                = left.y;
    this->rx                                = right.x;
    this->ry                                = right.y;
    this->l2                                = b.getLeftTrigger();
    this->r2                                = b.getRightTrigger();
    this->battery                           = static_cast<int>(b.getBatteryPercentage());
    bool had_jack                           = this->jack;
    bool had_link                           = this->plugged_usb_data;
    this->jack                              = b.isHeadset();
    this->plugged_usb_data                  = b.isPluggedUsbData();
    if (!this->plugged_usb_data) this->controller_disabled = false;
    if (this->plugged_usb_data != had_link) {
      this->led_force = true;
      serviceLeds();
    }
    if (!was_connected || had_jack != this->jack) applyOutputRoute();
    if (!was_connected) {
      if (this->paused_by_disconnect) {
        this->paused_by_disconnect = false;
        this->paused_by_jack       = false;
        ensureAudibleVolume();
        if (this->paused) {
          this->paused = false;
          playerSend("pause 0\n");
          this->playhead_dirty = true;
        }
      } else if (this->jack && this->paused_by_jack) {
        this->paused_by_jack = false;
        ensureAudibleVolume();
        if (this->paused) {
          this->paused = false;
          playerSend("pause 0\n");
          this->playhead_dirty = true;
        }
      }
    }
    this->temp = b.getTemperatureCentigrade();
    snprintf(this->dpad, sizeof(this->dpad), "%s", dpadName(b.getDpad()));
    buttonsToString(b.getButtons(), this->buttons, sizeof(this->buttons));
    this->raw[0] = 0;
    size_t r     = len < (sizeof(this->raw) - 1) / 2 ? len : (sizeof(this->raw) - 1) / 2;
    for (size_t i = 0; i < r; i++)
      snprintf(this->raw + i * 2, sizeof(this->raw) - i * 2, "%02x", raw_buf[i]);
    {
      using PowerState = dualsense::InputBody::PowerState;
      PowerState power = b.getPowerState();
      snprintf(this->charging, sizeof(this->charging), "%s",
               power == PowerState::Charging      ? "charging"
               : power == PowerState::Complete    ? "full"
               : power == PowerState::Discharging ? "discharging"
                                                  : "unknown");
      snprintf(this->powerState, sizeof(this->powerState), "%s",
               power == PowerState::Discharging           ? "discharging"
               : power == PowerState::Charging            ? "charging"
               : power == PowerState::Complete            ? "complete"
               : power == PowerState::AbnormalVoltage     ? "abnormalVoltage"
               : power == PowerState::AbnormalTemperature ? "abnormalTemperature"
               : power == PowerState::ChargingError       ? "chargingError"
                                                          : "unknown");
    }
    const dualsense::InputBody::Gyro gyro           = b.getGyro();
    this->gx                                        = gyro.x;
    this->gy                                        = gyro.y;
    this->gz                                        = gyro.z;
    const dualsense::InputBody::Accelerometer accel = b.getAccelerometer();
    this->ax                                        = accel.x;
    this->ay                                        = accel.y;
    this->az                                        = accel.z;
    this->host_timestamp                            = b.getHostTime();
    this->device_timestamp                          = b.getDeviceTime();
    this->trigger_effect_r = static_cast<unsigned>(b.getRightTriggerEffect());
    this->trigger_effect_l = static_cast<unsigned>(b.getLeftTriggerEffect());
    this->trigger_stop_r   = b.getRightTriggerStopLocation();
    this->trigger_stop_l   = b.getLeftTriggerStopLocation();
    this->trigger_status_r = static_cast<unsigned>(b.getRightTriggerStatus());
    this->trigger_status_l = static_cast<unsigned>(b.getLeftTriggerStatus());
    for (unsigned t = 0; t < 2; ++t) {
      const dualsense::InputBody::TouchPoint touch = b.getTouch(t);
      this->touch_active[t]                        = touch.isActive() ? 1 : 0;
      this->touch_id[t]                            = touch.getId();
      this->touch_x[t]                             = touch.getX();
      this->touch_y[t]                             = touch.getY();
    }

    long now = nowMs();
    if (this->rate_at == 0) this->rate_at = now;
    this->rate_frames++;
    if (now - this->rate_at >= 1000) {
      this->rate        = static_cast<int>(((this->rate_frames * 1000) / (now - this->rate_at)));
      this->rate_frames = 0;
      this->rate_at     = now;
    }
  }

  void inputAction(const char *name, int value) {
    // Manual shared-host fallback: ignore bindings while the user has disabled
    // controller input. The flag is only meaningful with a USB data link.
    if (this->padwaveDisabled()) return;
    handleAction(name, value);
  }

  void inputEvent(const char *) {
    if (this->padwaveDisabled()) return;
    if (this->idle_warning && nowMs() < this->idle_activity_ignore_until) return;
    noteActivity();
  }

  void inputMic(const int16_t *pcm, size_t samples) {
    if (!this->mic_monitor || samples == 0) return;

    long long sum = 0;
    int peak      = 0;
    for (size_t i = 0; i < samples; i++) {
      int v = pcm[i];
      sum += static_cast<long long>(v) * v;
      int a = v < 0 ? -v : v;
      if (a > peak) peak = a;
    }
    int rms   = static_cast<int>(sqrt(static_cast<double>(sum) / static_cast<double>(samples)));
    double db = rms > 0 ? 20.0 * log10(static_cast<double>(rms) / 32768.0) : -90.0;
    int level = static_cast<int>(((db + 60.0) * 100.0 / 60.0));
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    this->mic_rms   = rms;
    this->mic_peak  = peak;
    this->mic_level = (this->mic_level * 2 + level) / 3;
    this->mic_packets++;
    this->mic_frames++;
    this->mic_samples += static_cast<unsigned long>(samples);
    this->mic_last_ms = nowMs();
  }

  void handleAction(const char *name, int value) {
    bool was_suspended = this->suspended;
    noteActivity();
    if (was_suspended && (strcmp(name, "toggle_pause") == 0 || strcmp(name, "play") == 0)) {
      snprintf(this->last_action, sizeof(this->last_action), "%s", name);
      return;
    }

    if (strcmp(name, "next") == 0)
      next();
    else if (strcmp(name, "next_random") == 0)
      nextRandom();
    else if (strcmp(name, "previous") == 0)
      previous();
    else if (strcmp(name, "shuffle_folder") == 0)
      shuffleFolder();
    else if (strcmp(name, "pause") == 0 || strcmp(name, "toggle_pause") == 0)
      togglePause();
    else if (strcmp(name, "play") == 0) {
      if (!this->playing || this->paused) togglePause();
    } else if (strcmp(name, "mute") == 0 || strcmp(name, "toggle_mute") == 0) {
      toggleMute();
    } else if (strcmp(name, "toggle_power_save") == 0) {
      setPowerSaveMode(!this->power_save_mode);
    } else if (strcmp(name, "volume") == 0) {
      setVolume(this->volume + value);
    } else if (strcmp(name, "seek") == 0) {
      seek(this->pos_ms + static_cast<long>(value) * 1000);
    } else if (strcmp(name, "suspend") == 0 || strcmp(name, "disconnect") == 0) {
      debugLog("action %s playing=%d paused=%d web=%d", name, this->playing, this->paused,
               this->web_audio_clients);
      if (!this->paused) {
        this->playing = false;
        this->paused  = true;
        playerSend("pause 1\n");
        this->playhead_dirty = true;
      }
    } else if (strcmp(name, "jack_unplugged") == 0) {
      // macOS-style: a real unplug pauses playback. The input layer only emits
      // this on a stable transition, so starting with no headset does not
      // trigger it.
      applyOutputRoute();
      if (this->playing && !this->paused) {
        this->paused         = true;
        this->paused_by_jack = true;
        playerSend("pause 1\n");
        this->playhead_dirty = true;
      }
    } else if (strcmp(name, "jack_plugged") == 0) {
      applyOutputRoute();
      if (this->paused_by_jack) {
        this->paused_by_jack = false;
        ensureAudibleVolume();
        if (this->paused) {
          this->paused = false;
          playerSend("pause 0\n");
          this->playhead_dirty = true;
        }
      }
    }
    snprintf(this->last_action, sizeof(this->last_action), "%s", name);
  }

  void configLoad() {
    char path[512];
    if (!configPath(path, sizeof(path))) return;
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[kMaxLiveUrl + 64];
    while (fgets(line, sizeof(line), f)) {
      char *eq = strchr(line, '=');
      if (!eq) continue;
      *eq         = 0;
      char *value = eq + 1;
      char *nl    = strchr(value, '\n');
      if (nl) *nl = 0;

      if (strcmp(line, "idle_timeout_ms") == 0) {
        long v                = atol(value);
        this->idle_timeout_ms = static_cast<int>((v < 1000 ? 1000 : (v > 86400000 ? 86400000 : v)));
      } else if (strcmp(line, "idle_warn_grace_ms") == 0) {
        long v                   = atol(value);
        this->idle_warn_grace_ms = static_cast<int>((v < 0 ? 0 : (v > 600000 ? 600000 : v)));
      } else if (strcmp(line, "poweroff_when_idle_ms") == 0) {
        long v                      = atol(value);
        this->poweroff_when_idle_ms = static_cast<int>((v < 0 ? 0 : (v > 86400000 ? 86400000 : v)));
      } else if (strcmp(line, "volume") == 0) {
        int v        = atoi(value);
        this->volume = v < 0 ? 0 : (v > 100 ? 100 : v);
      } else if (strcmp(line, "last_nonzero_volume") == 0) {
        int v                     = atoi(value);
        this->last_nonzero_volume = v < 0 ? 0 : (v > 100 ? 100 : v);
      } else if (strcmp(line, "speed") == 0) {
        int v       = atoi(value);
        this->speed = v < 50 ? 50 : (v > 200 ? 200 : v);
      } else if (strcmp(line, "playback_mode") == 0) {
        snprintf(this->playback_mode, sizeof(this->playback_mode), "%s", value);
      } else if (strcmp(line, "stream_url") == 0) {
        snprintf(this->stream_url, sizeof(this->stream_url), "%s", value);
      } else if (strcmp(line, "live_url") == 0) {
        snprintf(this->live_url, sizeof(this->live_url), "%s", value);
      } else if (strcmp(line, "live_direct_url") == 0) {
        snprintf(this->live_direct_url, sizeof(this->live_direct_url), "%s", value);
      } else if (strcmp(line, "live_proxy") == 0) {
        snprintf(this->live_proxy, sizeof(this->live_proxy), "%s", value);
      } else if (strcmp(line, "live_proxy_user") == 0) {
        snprintf(this->live_proxy_user, sizeof(this->live_proxy_user), "%s", value);
      } else if (strcmp(line, "live_proxy_pass") == 0) {
        snprintf(this->live_proxy_pass, sizeof(this->live_proxy_pass), "%s", value);
      } else if (strcmp(line, "live_insecure") == 0) {
        this->live_insecure = strcmp(value, "true") == 0 || atoi(value) != 0;
      } else if (strcmp(line, "controller_mac") == 0) {
        snprintf(this->saved_controller_mac, sizeof(this->saved_controller_mac), "%s", value);
      } else if (strcmp(line, "play_path") == 0) {
        snprintf(this->resume_path, sizeof(this->resume_path), "%s", value);
      } else if (strcmp(line, "play_pos_ms") == 0) {
        this->resume_pos_ms = atol(value);
      } else if (strcmp(line, "haptics_only") == 0) {
        this->haptics_only = strcmp(value, "true") == 0 || atoi(value) != 0;
      } else if (strcmp(line, "power_save_mode") == 0) {
        if (strcmp(value, "true") == 0 || atoi(value) != 0) setPowerSaveMode(true, false);
      }
    }
    fclose(f);
  }

  void configSave() {
    char path[512];
    if (!configPath(path, sizeof(path))) return;

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
    fprintf(f, "poweroff_when_idle_ms=%d\n", this->poweroff_when_idle_ms);
    fprintf(f, "haptic_subwoofer=%s\n", this->haptic_subwoofer ? "true" : "false");
    fprintf(f, "haptic_cutoff_hz=%d\n", this->haptic_cutoff_hz);
    fprintf(f, "haptic_gain=%d\n", this->haptic_gain);
    const char *play_path = this->current >= 0 ? this->tracks[this->current].path : "";
    fputs("play_path=", f);
    for (const char *p = play_path; *p; p++) fputc((*p == '\n' || *p == '\r') ? ' ' : *p, f);
    fputc('\n', f);
    fprintf(f, "play_pos_ms=%ld\n", this->pos_ms);
    fprintf(f, "volume=%d\n", this->volume);
    fprintf(f, "last_nonzero_volume=%d\n", this->last_nonzero_volume);
    fprintf(f, "speed=%d\n", this->speed);
    fprintf(f, "playback_mode=%s\n", this->playback_mode);
    fprintf(f, "stream_url=%s\n", this->stream_url);
    fprintf(f, "live_url=%s\n", this->live_url);
    fprintf(f, "live_direct_url=%s\n", this->live_direct_url);
    fprintf(f, "live_proxy=%s\n", this->live_proxy);
    fprintf(f, "live_proxy_user=%s\n", this->live_proxy_user);
    fprintf(f, "live_proxy_pass=%s\n", this->live_proxy_pass);
    fprintf(f, "live_insecure=%s\n", this->live_insecure ? "true" : "false");
    fprintf(f, "controller_mac=%s\n", this->saved_controller_mac);
    fprintf(f, "haptics_only=%s\n", this->haptics_only ? "true" : "false");
    fprintf(f, "power_save_mode=%s\n", this->power_save_mode ? "true" : "false");
    fclose(f);
    this->playhead_saved_at = nowMs();
  }

  void noteActivity() {
    this->last_activity_ms = nowMs();
    this->idle_since       = this->last_activity_ms;
    if (this->idle_warning || this->suspended) endIdle();
  }

  void endIdle() {
    if (this->idle_volume_saved) {
      char line[32];
      snprintf(line, sizeof(line), "volume %d\n", this->idle_volume);
      playerSend(line);
      this->idle_volume_saved = false;
    }
    if (this->paused_by_idle) {
      this->paused_by_idle = false;
      if (this->paused) {
        this->paused = false;
        playerSend("pause 0\n");
        this->playhead_dirty = true;
      }
    }
    this->idle_warning     = false;
    this->suspended        = false;
    this->idle_fade_volume = -1;
    this->poweroff_at      = 0;
    this->powered_off      = false;
  }

  void updateIdle() {
    long now = nowMs();
    // The idle warning/fade exists to protect the controller. Without one,
    // playback may be browser-only and should keep running indefinitely.
    if (!this->controller) {
      if (this->idle_warning || this->suspended) endIdle();
      return;
    }
    if (this->suspended && this->poweroff_at && now >= this->poweroff_at) {
      this->poweroff_at = 0;
      configSave();
      this->powered_off = powerOffController();
    }
    if (this->suspended) return;

    long idle_ms         = now > this->last_activity_ms ? now - this->last_activity_ms : 0;
    bool active_playback = this->playing && !this->paused;

    if (!active_playback) {
      if (idle_ms < this->idle_timeout_ms) return;
      this->idle_warning   = false;
      this->suspended      = true;
      this->paused_by_idle = true;
      this->paused         = true;
      this->playhead_dirty = true;
      playerSend("pause 1\n");
      this->poweroff_at = 0;
      this->powered_off = powerOffController();
      this->led_force   = true;
      serviceLeds();
      return;
    }

    long timeout = this->idle_timeout_ms;
    long grace   = this->idle_warn_grace_ms;

    if (!this->idle_warning && idle_ms >= timeout) {
      this->idle_warning      = true;
      this->idle_volume       = this->volume;
      this->idle_volume_saved = true;
      this->idle_fade_volume  = this->volume;
      this->pulseIdleWarning();
    }

    if (!this->idle_warning) return;

    long over    = idle_ms - timeout;
    int progress = grace > 0 ? static_cast<int>((over * 100 / grace)) : 100;
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;

    int floor = this->idle_volume * 10 / 100;
    if (floor < 1 && this->idle_volume > 0) floor = 1;
    int target = this->idle_volume - (this->idle_volume - floor) * progress / 100;
    if (target != this->idle_fade_volume) {
      this->idle_fade_volume = target;
      char line[32];
      snprintf(line, sizeof(line), "volume %d\n", target);
      playerSend(line);
    }

    if (over >= grace) {
      this->idle_warning     = false;
      this->suspended        = true;
      this->paused_by_idle   = true;
      this->paused           = true;
      this->idle_fade_volume = 0;
      playerSend("volume 0\n");
      playerSend("pause 1\n");
      this->playhead_dirty = true;
      if (this->poweroff_when_idle_ms > 0) this->poweroff_at = now + this->poweroff_when_idle_ms;
    }
  }

  void setPowerSaveMode(bool on, bool persist = true) {
    if (this->power_save_mode == on) return;
    if (on) {
      this->power_save_saved_mask   = this->power_save_mask;
      this->lightbar_saved_on       = this->lightbar_on;
      this->power_save_mask         = kPowerSaveModeMask; // touch, motion, haptic, mic mute
      this->power_save_mode         = true;
      this->lightbar_on             = false;
      this->power_save_leds_applied = false;
      this->led_force               = true;
      serviceLeds();
    } else {
      this->power_save_mode         = false;
      this->power_save_mask         = this->power_save_saved_mask;
      this->lightbar_on             = this->lightbar_saved_on;
      this->power_save_leds_applied = false;
      char fields[48];
      snprintf(fields, sizeof(fields), "power_save_control=%d", this->power_save_mask);
      outputFields(fields);
      this->led_force       = true;
      this->led_last_rgb[0] = this->led_last_rgb[1] = this->led_last_rgb[2] = -1;
      this->led_last_player                                                 = -2;
      this->led_last_state                                                  = -1;
      this->led_painted                                                     = false;
      serviceLeds();
    }
    if (persist) configSave();
  }

  bool powerOffController() {
    // Keep the pad awake whenever USB data or USB power is present, even if
    // padwave is using the Bluetooth transport.
    if (this->player.isUsb() || this->plugged_usb_data || this->pluggedUsbPower) return false;
    char mac[32] = "";
    if (this->controller_mac[0]) {
      snprintf(mac, sizeof(mac), "%s", this->controller_mac);
    } else {
      DualSenseDevice device{};
      if (!device.isValid()) return false;
      snprintf(mac, sizeof(mac), "%s", device.mac);
    }

    char command[160];
    snprintf(command, sizeof(command), "timeout 5 bluetoothctl disconnect %s >/dev/null 2>&1", mac);
    int rc = system(command);
    if (rc != 0) {
      fprintf(stderr, "padwave: bluetoothctl disconnect %s failed (%d)\n", mac, rc);
      return false;
    }
    fprintf(stderr, "padwave: powered off controller %s after idle\n", mac);
    return true;
  }

  void setLightbarSetup(dualsense::OutputBody::LightbarSetup setup) {
    if (this->padwaveDisabled()) return;
    reclaimLeds();
    if (setup == dualsense::OutputBody::LightbarSetup::LightOn) this->led_force = true;
    char fields[32];
    snprintf(fields, sizeof(fields), "lightbar_setup=%d", static_cast<int>(setup));
    outputFields(fields);
    playerSend("state_clear_lightbar\n");
  }

  void outputFields(const char *fields) {
    if (!this->player) return;
    char line[1200];
    snprintf(line, sizeof(line), "fields %s\n", fields);
    playerSend(line);
  }

  int ledState() const {
    if (!this->controller) return 0;
    if (this->suspended || this->powered_off || !this->playing) return 0;
    if (this->idle_warning) return 1;
    if (this->volume == 0) return 2;
    if (this->paused) return 3;
    return 4;
  }

  static const char *ledStateName(int state) {
    switch (state) {
    case 1: return "warning";
    case 2: return "muted";
    case 3: return "paused";
    case 4: return "playing";
    default: return "off";
    }
  }

  void reclaimLeds() {
    this->led_released = false;
    this->led_force    = true;
  }

  bool applyLightbar(int r, int g, int b, bool force) {
    if (!this->player || this->padwaveDisabled()) return false;
    if (!force && this->led_last_rgb[0] == r && this->led_last_rgb[1] == g &&
        this->led_last_rgb[2] == b)
      return true;
    char fields[160];
    snprintf(fields, sizeof(fields), "lightbar_rgb=%d,%d,%d", r, g, b);
    outputFields(fields);
    this->led_last_rgb[0] = r;
    this->led_last_rgb[1] = g;
    this->led_last_rgb[2] = b;
    return true;
  }

  bool applyPlayerLeds(int mask, bool force) {
    if (!this->player) return false;
    if (!force && this->led_last_player == mask) return true;
    char fields[96];
    snprintf(fields, sizeof(fields), "player_leds=%d", mask);
    outputFields(fields);
    this->led_last_player = mask;
    return true;
  }

  void serviceLeds() {
    if (this->led_released) return;
    if (this->power_save_mode) {
      if (!this->controller || !this->player) {
        this->power_save_leds_applied = false;
        return;
      }
      if (!this->power_save_leds_applied && !this->padwaveDisabled()) {
        this->power_save_leds_applied = true;
        char fields[192];
        snprintf(fields, sizeof(fields),
                 "power_save_control=%d;lightbar_setup=2;player_led_brightness=0;"
                 "player_leds=0;lightbar_rgb=0,0,0;"
                 "mute_button_led=0",
                 this->power_save_mask);
        outputFields(fields);
      }
      return;
    }
    this->power_save_leds_applied = false;

    long now  = nowMs();
    int state = ledState();

    if (state != this->led_last_state) {
      bool was_off = this->led_last_state == 0;
      bool is_off  = state == 0;
      // A custom colour survives device-down/up, but a playback/session state
      // change takes the bar back.
      if (!was_off && !is_off) this->led_custom_set = false;
      this->led_last_state = state;
      if (state == 1) {
        this->led_warning_high = true;
        this->led_warning_at   = now;
      }
    }

    if (state == 1 && !this->led_custom_set && now - this->led_warning_at >= 600) {
      this->led_warning_high = !this->led_warning_high;
      this->led_warning_at   = now;
    }

    bool charging_now = this->controller && strcmp(this->charging, "charging") == 0;
    if (charging_now && this->lightbar_on && !this->padwaveDisabled()) {
      if (this->led_charge_at == 0) {
        // Start the charging loop with setup 2, then 1 two seconds later.
        this->led_charge_at    = now;
        this->led_charge_setup = dualsense::OutputBody::LightbarSetup::LightOut;
        setLightbarSetup(dualsense::OutputBody::LightbarSetup::LightOut);
      } else if (now - this->led_charge_at >= kChargeBlinkMs) {
        this->led_charge_at = now;
        this->led_charge_setup =
            this->led_charge_setup == dualsense::OutputBody::LightbarSetup::LightOut
                ? dualsense::OutputBody::LightbarSetup::LightOn
                : dualsense::OutputBody::LightbarSetup::LightOut;
        setLightbarSetup(this->led_charge_setup);
      }
    } else if (this->led_charge_at != 0) {
      this->led_charge_at    = 0;
      this->led_charge_setup = dualsense::OutputBody::LightbarSetup::LightOut;
      this->led_force        = this->lightbar_on;
      setLightbarSetup(this->lightbar_on ? dualsense::OutputBody::LightbarSetup::LightOn
                                         : dualsense::OutputBody::LightbarSetup::LightOut);
    }

    int r = 0, g = 0, b = 0;
    if (state == 0) {
      // Device down / suspended / stopped: dark, even with a custom colour.
      r = 0;
      g = 0;
      b = 0;
    } else if (this->led_custom_set) {
      r = this->led_custom[0];
      g = this->led_custom[1];
      b = this->led_custom[2];
    } else if (state == 1) {
      r = this->led_warning_high ? 255 : 60;
    } else if (state == 2) {
      r = 120;
      g = 60;
      b = 255;
    } else if (state == 3) {
      r = 255;
      g = 110;
      b = 0;
    } else if (state == 4) {
      r = 0;
      g = 140;
      b = 255;
    }

    bool is_charging = this->controller && (strcmp(this->charging, "charging") == 0 ||
                                            strcmp(this->charging, "full") == 0);

    // Five player LEDs as a left-to-right battery bar. Half is 11100, full is
    // 11111. Only a near-empty battery blinks (first LED, 10000/00000); charging
    // is always the solid bar so it cannot distract.
    int mask = 0;
    if (this->controller && this->battery >= 0) {
      int lit = (this->battery * 5 + 99) / 100;
      if (lit < 1) lit = 1;
      if (lit > 5) lit = 5;
      if (!is_charging && this->battery <= 15) {
        if (now - this->led_battery_at >= kLedBlinkMs) {
          this->led_battery_at = now;
          this->led_battery_phase ^= 1;
        }
        mask = this->led_battery_phase ? 0 : 1;
      } else {
        this->led_battery_phase = 0;
        for (int i = 0; i < lit; i++) mask |= 1 << i;
      }
    }
    if (this->led_manual_player) mask = this->led_manual_mask;

    // Charging owns the lightbar: setup 2/1 alternate every 2 s with no RGB
    // frames; full is solid white. Both override playback/custom colours.
    if (this->controller && strcmp(this->charging, "charging") == 0) {
      r = 255;
      g = 110;
      b = 0;
    } else if (this->controller && strcmp(this->charging, "full") == 0) {
      r = 255;
      g = 255;
      b = 255;
    }

    bool force   = this->led_force;
    bool rgb_ok  = this->lightbar_on && !charging_now && !this->padwaveDisabled() &&
                   applyLightbar(r, g, b, force);
    bool mask_ok = applyPlayerLeds(mask, force);
    if (rgb_ok || mask_ok) this->led_painted = true;
    if (mask_ok) this->led_force = false;
  }

  void pulseIdleWarning() {
    if (!this->player || !this->controller || this->padwaveDisabled()) return;
    if (this->rumble_until || this->idle_rumble_until) return;
    outputFields("motor_left=64;motor_right=64");
    this->idle_rumble_until          = nowMs() + 300;
    // The controller's own accelerometer reports the pulse as motion; do not
    // let that motion cancel the warning/fade.
    this->idle_activity_ignore_until = nowMs() + 1200;
  }

  void serviceOutput() {
    if (!this->player) return;
    long now = nowMs();
    if (this->idle_rumble_until && now >= this->idle_rumble_until) {
      if (!this->rumble_until) outputFields("motor_left=0;motor_right=0");
      this->idle_rumble_until = 0;
    }
    if (this->rumble_until && now >= this->rumble_until) {
      outputFields("motor_left=0;motor_right=0");
      this->rumble_until = 0;
    }
  }

  int command(const char *action, const char *body, size_t body_len, char *out, size_t out_cap) {
    (void)body_len;

    bool was_suspended = this->suspended;
    noteActivity();

    bool file_action = strcmp(action, "next") == 0 || strcmp(action, "previous") == 0 ||
                       strcmp(action, "shuffle_folder") == 0 || strcmp(action, "pause") == 0 ||
                       strcmp(action, "seek") == 0 || strcmp(action, "seek_to") == 0 ||
                       strcmp(action, "play_track") == 0 || strcmp(action, "resume") == 0;
    if (this->live_active && file_action) stopLive();

    if (strcmp(action, "resume") == 0) {
      if (this->live_active) stopLive();
      if (!this->player) playerStart();
      int index = this->current;
      if (index < 0 && this->count > 0) index = 0;
      if (index >= 0 && this->player) {
        if (!this->player.isPlaying() || this->paused || !this->playing)
          play(index, this->pos_ms);
      }
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return snprintf(out, out_cap, "ok=true\n");
    }
    if (strcmp(action, "controller_reconnect") == 0) {
      if (this->controller && this->player.fd() >= 0) {
        snprintf(this->last_action, sizeof(this->last_action), "%s", action);
        return snprintf(out, out_cap, "ok=true\nalready=true\n");
      }
      const char *mac = this->saved_controller_mac[0] ? this->saved_controller_mac
                                                      : this->controller_mac;
      if (!mac[0])
        return snprintf(out, out_cap, "ok=false\nerror=no saved controller MAC\n");
      for (const char *p = mac; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F') || *p == ':'))
          return snprintf(out, out_cap, "ok=false\nerror=invalid controller MAC\n");
      char cmd[256];
      snprintf(cmd, sizeof(cmd),
               "timeout 5 bluetoothctl trust %s >/dev/null 2>&1; "
               "timeout 10 bluetoothctl connect %s >/dev/null 2>&1",
               mac, mac);
      int rc = system(cmd);
      this->inputStart();
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return snprintf(out, out_cap, "ok=%s\nmac=%s\n", rc == 0 ? "true" : "false", mac);
    }
    if (strcmp(action, "live_url") == 0) {
      char url[kMaxLiveUrl];
      if (!kvGet(body, "value", url, sizeof(url)))
        return snprintf(out, out_cap, "ok=false\nerror=missing value\n");
      if (url[0] && !validLiveUrl(url))
        return snprintf(out, out_cap, "ok=false\nerror=http(s) URL required\n");
      stopLive();
      char cache[512];
      if (liveCachePath(cache, sizeof(cache))) unlink(cache);
      snprintf(this->live_url, sizeof(this->live_url), "%s", url);
      this->live_count    = 0;
      this->live_error[0] = 0;
      configSave();
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return snprintf(out, out_cap, "ok=true\n");
    }
    if (strcmp(action, "live_refresh") == 0) {
      bool ok = refreshLive();
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return ok ? snprintf(out, out_cap, "ok=true\ncount=%d\n", this->live_count)
                : snprintf(out, out_cap, "ok=false\nerror=%s\n", this->live_error);
    }
    if (strcmp(action, "live_play") == 0) {
      int id = static_cast<int>(kvNumber(body, "id", -1));
      if (!startLive(id - 1))
        return snprintf(out, out_cap, "ok=false\nerror=%s\n", this->live_error);
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return snprintf(out, out_cap, "ok=true\n");
    }
    if (strcmp(action, "live_direct") == 0) {
      char url[kMaxLiveUrl];
      if (!kvGet(body, "value", url, sizeof(url)) || !validLiveUrl(url))
        return snprintf(out, out_cap, "ok=false\nerror=http(s) URL required\n");
      snprintf(this->live_direct_url, sizeof(this->live_direct_url), "%s", url);
      configSave();
      if (!startLiveDirect(url))
        return snprintf(out, out_cap, "ok=false\nerror=%s\n", this->live_error);
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return snprintf(out, out_cap, "ok=true\n");
    }
    if (strcmp(action, "live_tls") == 0) {
      this->live_insecure = kvNumber(body, "value", 0) != 0;
      configSave();
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return snprintf(out, out_cap, "ok=true\n");
    }
    if (strcmp(action, "live_proxy") == 0) {
      char url[512], user[128], pass[128];
      if (!kvGet(body, "url", url, sizeof(url)))
        return snprintf(out, out_cap, "ok=false\nerror=missing url\n");
      if (!validProxyUrl(url))
        return snprintf(out, out_cap, "ok=false\nerror=unsupported proxy URL\n");
      snprintf(this->live_proxy, sizeof(this->live_proxy), "%s", url);
      if (!url[0]) {
        this->live_proxy_user[0] = 0;
        this->live_proxy_pass[0] = 0;
      } else {
        if (kvGet(body, "user", user, sizeof(user)))
          snprintf(this->live_proxy_user, sizeof(this->live_proxy_user), "%s", user);
        if (kvGet(body, "pass", pass, sizeof(pass)))
          snprintf(this->live_proxy_pass, sizeof(this->live_proxy_pass), "%s", pass);
      }
      configSave();
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return snprintf(out, out_cap, "ok=true\n");
    }
    if (strcmp(action, "live_stop") == 0) {
      stopLive();
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return snprintf(out, out_cap, "ok=true\n");
    }

    if (this->sink_playback && file_action) sinkPlaybackStop();
    if (was_suspended && strcmp(action, "pause") == 0) {
      snprintf(this->last_action, sizeof(this->last_action), "%s", action);
      return snprintf(out, out_cap, "ok=true\n");
    }

    if (strcmp(action, "next") == 0)
      next();
    else if (strcmp(action, "previous") == 0)
      previous();
    else if (strcmp(action, "shuffle_folder") == 0)
      shuffleFolder();
    else if (strcmp(action, "pause") == 0)
      togglePause();
    else if (strcmp(action, "mute") == 0)
      toggleMute();
    else if (strcmp(action, "disconnect") == 0) {
      stopLive();
      sinkPlaybackStop();
      this->playing = false;
      this->paused  = true;
      playerSend("pause 1\n");
    } else if (strcmp(action, "volume") == 0) {
      setVolume(static_cast<int>(kvNumber(body, "value", this->volume)));
    } else if (strcmp(action, "speed") == 0) {
      setSpeed(static_cast<int>(kvNumber(body, "value", this->speed)));
    } else if (strcmp(action, "seek") == 0) {
      seek(this->pos_ms + kvNumber(body, "delta", 0) * 1000);
    } else if (strcmp(action, "seek_to") == 0) {
      seek(kvNumber(body, "position", this->pos_ms / 1000) * 1000);
    } else if (strcmp(action, "favorite") == 0) {
      char id[64];
      if (!kvGet(body, "id", id, sizeof(id)))
        return snprintf(out, out_cap, "ok=false\nerror=missing id\n");
      bool value = false;
      if (!toggleFavorite(id, &value))
        return snprintf(out, out_cap, "ok=false\nerror=track not found\n");
      return snprintf(out, out_cap, "ok=true\nfavorite=%s\n", value ? "true" : "false");
    } else if (strcmp(action, "play_track") == 0) {
      char id[64];
      if (kvGet(body, "id", id, sizeof(id))) {
        int idx = findId(id);
        if (idx >= 0) play(idx, 0);
      }
    } else {
      return snprintf(out, out_cap, "ok=false\nerror=unknown action\n");
    }

    snprintf(this->last_action, sizeof(this->last_action), "%s", action);
    return snprintf(out, out_cap, "ok=true\n");
  }
};
