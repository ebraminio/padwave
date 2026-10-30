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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "padwave_player.hh"
#include "padwave_input.hh"

// The Bluetooth address from the hidraw uevent's HID_UNIQ. bluetoothctl and
// the kernel both spell it lower-case with colons.

// The app owns the library, config and text helpers.
// ---------------------------------------------------------------------------
// The app
// ---------------------------------------------------------------------------

struct App {
  friend struct Player;
  friend struct Input;

private:
  static constexpr int kMaxTracks = 4096;
  static constexpr int kMaxPath = 1024;
  static constexpr int kMaxTitle = 160;
  static constexpr int kMaxFolder = 96;
  static constexpr int kMaxHistory = 48;

  struct Track {
    char path[kMaxPath];
    char title[kMaxTitle];
    char folder[kMaxFolder];
    char ext[8];
    char id[17];
  };

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
    int c = naturalCompare(x->folder, y->folder);
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
      *len = start;
      buf[start] = 0;
      return;
    }
    *len += static_cast<size_t>(m);

    buf[(*len)++] = '\n';
    buf[*len] = 0;
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
    buf[*len] = 0;
  }

  static bool kvGet(const char *body, const char *key, char *out, size_t cap) {
    if (!body || cap == 0) return false;
    size_t klen = strlen(key);
    for (const char *p = body; *p;) {
      const char *eol = strchr(p, '\n');
      size_t n = eol ? static_cast<size_t>((eol - p)) : strlen(p);
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

  static void tsvField(char *buf, size_t cap, size_t *o, const char *value, char sep) {
    for (const char *p = value; p && *p && *o + 2 < cap; p++)
      buf[(*o)++] = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
    if (*o + 1 < cap) buf[(*o)++] = sep;
    buf[*o] = 0;
  }

  Track tracks[kMaxTracks];
  int count = 0;
  int current = -1;
  int history[kMaxHistory];
  int history_len = 0;

  Player player;
  Input input;
  int player_timer_fd = -1;

  static constexpr int kDefaultVolume = 15;

  long pos_ms = 0;
  long dur_ms = 0;
  long sent = 0;
  long underruns = 0;
  bool eof = false;
  bool playing = false;
  bool paused = false;
  bool paused_by_volume = false;
  int volume = kDefaultVolume;
  int last_nonzero_volume = kDefaultVolume;
  int speed = 100;

  bool controller = false;
  bool jack = false;
  bool usb_data_link = false;
  int battery = -1;
  char charging[16] = "unknown";
  int lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
  char dpad[16] = "none";
  char buttons[256] = "";
  int temp = 0;
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  unsigned seq = 0;
  int rate = 0;
  unsigned tfl = 0, tfr = 0;
  unsigned touch_active[2] = {};
  unsigned touch_id[2] = {};
  unsigned touch_x[2] = {};
  unsigned touch_y[2] = {};
  long rate_at = 0;
  unsigned rate_frames = 0;
  char input_node[64] = "";
  char controller_mac[32] = "";
  char last_action[64] = "";
  char raw[400] = "";

  // Where the last session left off.
  char resume_path[kMaxPath] = "";
  long resume_pos_ms = 0;
  bool resume_pending = false;
  bool playhead_dirty = false;
  long playhead_saved_at = 0;
  bool volume_dirty = false;
  long volume_saved_at = 0;

  int reconnect_at = 0;
  long idle_since = 0;
  long last_activity_ms = 0;
  bool idle_warning = false;
  bool suspended = false;
  bool paused_by_idle = false;
  bool paused_by_jack = false;
  bool paused_by_disconnect = false;
  bool paused_by_mic = false;
  bool idle_volume_saved = false;
  int idle_volume = 0;
  int idle_fade_volume = -1;
  long poweroff_at = 0;
  bool powered_off = false;

  // Config that the panel edits; persisted as ~/.config/padwave/config.
  int idle_timeout_ms = 900000;
  int idle_warn_grace_ms = 10000;
  int poweroff_when_idle_ms = 60000;
  int trigger_effect_amplitude = 6;
  int trigger_effect_position = 2;
  int trigger_effect_frequency = 25;
  int trigger_effect_end = 9;
  int trigger_effect_period = 10;
  int trigger_effect_amplitude_b = 0;
  bool trigger_effects = false;
  char trigger_effect_preset[32] = "none";

  int rumble_left = 0, rumble_right = 0;
  long rumble_until = 0;
  int trigger_side = 0; // 1 left, 2 right; 0 idle
  long trigger_until = 0;

  // DS4Windows Trigger Lab's built-in presets. Percent values are converted
  // the same way: start/end to zones 0-9, force to 1-8, wall to Hz for 0x26.
  struct TriggerPreset {
    const char *id;
    uint8_t mode;
    int start_pct;
    int wall_pct;
    int force_pct;
  };

  static constexpr TriggerPreset kTriggerPresets[] = {
      {"none", 0x05, 0, 0, 0},
      {"balanced-weapon", 0x25, 20, 60, 85},
      {"soft-resistance", 0x21, 10, 50, 30},
      {"firm-resistance", 0x21, 5, 50, 75},
      {"hair-trigger", 0x25, 5, 25, 65},
      {"pistol-break", 0x25, 20, 55, 95},
      {"rifle-break", 0x25, 35, 75, 100},
      {"machine-gun", 0x26, 15, 80, 85},
      {"road-texture", 0x26, 5, 35, 45},
  };

  // The state-coloured lightbar and the player-LED ownership. The player's
  // 0x36 snapshot carries whatever `fields` last merged into its OutputBody.
  int led_custom[3] = {0, 0, 0};
  bool led_custom_set = false;
  bool led_force = true;
  int led_last_rgb[3] = {-1, -1, -1};
  int led_last_player = -2;
  int led_last_state = -1;
  bool led_warning_high = true;
  long led_warning_at = 0;
  long led_battery_at = 0;
  int led_battery_phase = 0;
  static constexpr long kLedBlinkMs = 2000;
  bool led_manual_player = false;
  int led_manual_mask = 4;
  bool led_painted = false;
  bool lightbar_on = true;
  long led_charge_at = 0;
  dualsense::OutputBody::LightbarSetup led_charge_setup =
      dualsense::OutputBody::LightbarSetup::LightOut;
  static constexpr long kChargeBlinkMs = 1000;

  unsigned long mic_packets = 0;
  unsigned long mic_frames = 0;
  unsigned long mic_samples = 0;
  int mic_rms = 0;
  int mic_peak = 0;
  int mic_level = 0;
  long mic_last_ms = 0;
  bool mic_monitor = false;

public:
  bool init() {
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
        scanDir(expanded, "");
      } else {
        scanDir(dir, "");
      }
    }

    qsort(this->tracks, static_cast<size_t>(this->count), sizeof(Track), compareTracks);
    srand(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid()));
    this->player_timer_fd = -1;
    configLoad();
    if (this->resume_path[0]) {
      int idx = findTrack(this->resume_path);
      if (idx >= 0) {
        this->current = idx;
        this->pos_ms = this->resume_pos_ms;
        this->resume_pending = true;
      }
    }
    this->playhead_saved_at = nowMs();
    this->last_activity_ms = this->playhead_saved_at;
    // configLoad() restored the current and last non-zero volumes. A current
    // non-zero value is by definition the latest one; only fall back to 15
    // when no non-zero value has ever been stored.
    if (this->volume > 0)
      this->last_nonzero_volume = this->volume;
    else if (this->last_nonzero_volume <= 0)
      this->last_nonzero_volume = kDefaultVolume;
    this->volume_dirty = false;
    return true;
  }

  void shutdown() {
    this->playing = false;
    this->controller = false;
    this->led_force = true;
    serviceLeds();
    configSave();
    if (this->player) {
      playerSend("quit\n");
      this->player.destroy();
    }
    this->player_timer_fd = -1;
    inputStop(false);
    micReset();
  }

  void tick() {
    // Invariant: playback running means the mic monitor is off, regardless of
    // which resume path got us here.
    serviceOutput();
    updateIdle();
    serviceLeds();
    if (this->eof) {
      this->eof = false;
      next();
    }
    if (!this->player) {
      playerStart();
      if (this->player) {
        if (this->resume_pending && this->current >= 0) {
          this->resume_pending = false;
          play(this->current, this->pos_ms);
        } else if (this->current < 0 && this->count > 0) {
          int start = rand() % this->count;
          play(start, 0);
        }
        if (this->controller && !this->jack) pauseIfNoJack();
      }
    }
    if (this->player && this->input.fd() >= 0 && this->input.fd() != this->player.fd()) {
      this->input.close();
      if (this->player.fd() >= 0) this->inputStart();
    }
    if (this->input.fd() < 0 && this->player && this->playing && !this->paused) {
      this->paused = true;
      this->paused_by_disconnect = true;
      playerSend("pause 1\n");
      this->playhead_dirty = true;
    }
    if (this->input.fd() < 0 && static_cast<int>(nowMs()) >= this->reconnect_at) inputStart();

    long now = nowMs();
    if (this->playhead_dirty && now - this->playhead_saved_at >= 1000) {
      this->playhead_dirty = false;
      this->playhead_saved_at = now;
      configSave();
    } else if (this->current >= 0 && now - this->playhead_saved_at >= 30000) {
      this->playhead_saved_at = now;
      configSave();
    }
    if (this->volume_dirty && now - this->volume_saved_at >= 1000) {
      this->volume_dirty = false;
      this->volume_saved_at = now;
      configSave();
    }
  }

  int fds(int *fds, int max) {
    int n = 0;
    if (this->player_timer_fd >= 0 && n < max) fds[n++] = this->player_timer_fd;
    if (this->input.fd() >= 0 && n < max) fds[n++] = this->input.fd();
    return n;
  }

  void handleFd(int fd) {
    if (this->player && fd == this->player_timer_fd) {
      this->player.step();
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

  int statusText(char *buf, size_t cap) {
    size_t o = 0;
    if (cap) buf[0] = 0;
    const Track *s = this->current >= 0 ? &this->tracks[this->current] : nullptr;

    long now = nowMs();
    long idle_ms = now > this->last_activity_ms ? now - this->last_activity_ms : 0;
    if (!this->playing || (this->paused && !this->suspended)) idle_ms = 0;
    long timeout = this->idle_timeout_ms;
    long grace = this->idle_warn_grace_ms;
    const char *phase = "active";
    long warning_in = (timeout + 999) / 1000;
    long shutdown_in = 0;
    int fade_percent = 100;

    if (this->suspended) {
      phase = "suspended";
      warning_in = 0;
      fade_percent = 10;
    } else if (this->idle_warning && idle_ms >= timeout) {
      phase = "warning";
      long over = idle_ms - timeout;
      int progress = grace > 0 ? static_cast<int>((over * 100 / grace)) : 100;
      if (progress < 0) progress = 0;
      if (progress > 100) progress = 100;
      fade_percent = 100 - progress * 90 / 100;
      warning_in = 0;
      long left = timeout + grace - idle_ms;
      shutdown_in = left > 0 ? (left + 999) / 1000 : 0;
    } else {
      long left = timeout - idle_ms;
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
    }

    kvText(buf, cap, &o, "session.phase", phase);
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

    kvAdd(buf, cap, &o, "audio.present", "%s", this->player.valid() ? "true" : "false");
    kvText(buf, cap, &o, "audio.sink", this->player.audioSink());
    kvText(buf, cap, &o, "audio.route", this->player.speakerOutput() ? "speaker" : "headphones");
    kvText(buf, cap, &o, "audio.state",
           this->player.audioActive() ? "running" : (this->player.valid() ? "ready" : "null"));

    kvText(buf, cap, &o, "hid.power_save", "null");
    kvAdd(buf, cap, &o, "hid.player_leds", "%d",
          this->led_last_player >= 0 ? this->led_last_player : 4);
    int state = ledState();
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
    kvText(buf, cap, &o, "controller.model", "dualsense");
    kvText(buf, cap, &o, "controller.node", this->input_node);
    if (this->battery >= 0)
      kvAdd(buf, cap, &o, "controller.battery", "%d", this->battery);
    else
      kvText(buf, cap, &o, "controller.battery", "null");
    kvText(buf, cap, &o, "controller.charging", this->charging);
    kvAdd(buf, cap, &o, "controller.jack", "%s", this->jack ? "true" : "false");
    kvAdd(buf, cap, &o, "controller.usb_data_link", "%s", this->usb_data_link ? "true" : "false");
    kvAdd(buf, cap, &o, "controller.usb_host", "%s", this->externalHost() ? "true" : "false");
    kvText(buf, cap, &o, "controller.buttons", this->buttons);
    kvText(buf, cap, &o, "controller.dpad", this->dpad);
    kvAdd(buf, cap, &o, "controller.rate", "%d", this->rate);
    kvAdd(buf, cap, &o, "controller.fields.seq", "%u", this->seq);
    kvAdd(buf, cap, &o, "controller.fields.tfl", "%u", this->tfl);
    kvAdd(buf, cap, &o, "controller.fields.tfr", "%u", this->tfr);
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
    kvAdd(buf, cap, &o, "config.trigger_effect_amplitude", "%d", this->trigger_effect_amplitude);
    kvAdd(buf, cap, &o, "config.trigger_effect_position", "%d", this->trigger_effect_position);
    kvAdd(buf, cap, &o, "config.trigger_effect_frequency", "%d", this->trigger_effect_frequency);
    kvAdd(buf, cap, &o, "config.trigger_effect_end", "%d", this->trigger_effect_end);
    kvAdd(buf, cap, &o, "config.trigger_effect_period", "%d", this->trigger_effect_period);
    kvAdd(buf, cap, &o, "config.trigger_effect_amplitude_b", "%d",
          this->trigger_effect_amplitude_b);
    kvText(buf, cap, &o, "config.trigger_effect_preset", this->trigger_effect_preset);
    kvAdd(buf, cap, &o, "config.trigger_effects", "%s", this->trigger_effects ? "true" : "false");
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
    kvAdd(buf, cap, &o, "config.trigger_effect_amplitude", "%d", this->trigger_effect_amplitude);
    kvAdd(buf, cap, &o, "config.trigger_effect_position", "%d", this->trigger_effect_position);
    kvAdd(buf, cap, &o, "config.trigger_effect_frequency", "%d", this->trigger_effect_frequency);
    kvAdd(buf, cap, &o, "config.trigger_effect_end", "%d", this->trigger_effect_end);
    kvAdd(buf, cap, &o, "config.trigger_effect_period", "%d", this->trigger_effect_period);
    kvAdd(buf, cap, &o, "config.trigger_effect_amplitude_b", "%d",
          this->trigger_effect_amplitude_b);
    kvText(buf, cap, &o, "config.trigger_effect_preset", this->trigger_effect_preset);
    kvAdd(buf, cap, &o, "config.trigger_effects", "%s", this->trigger_effects ? "true" : "false");
    kvAdd(buf, cap, &o, "config.volume", "%d", this->volume);
    kvAdd(buf, cap, &o, "config.last_nonzero_volume", "%d", this->last_nonzero_volume);
    kvAdd(buf, cap, &o, "config.speed", "%d", this->speed);
    return static_cast<int>(o);
  }

  void micReset() {
    this->mic_packets = 0;
    this->mic_frames = 0;
    this->mic_samples = 0;
    this->mic_rms = 0;
    this->mic_peak = 0;
    this->mic_level = 0;
    this->mic_last_ms = 0;
  }

  void configSet(const char *body) {
    noteActivity();
    char value[64];

    if (kvGet(body, "idle_timeout_ms", value, sizeof(value))) {
      long v = atol(value);
      this->idle_timeout_ms = static_cast<int>((v < 1000 ? 1000 : (v > 86400000 ? 86400000 : v)));
    }
    if (kvGet(body, "idle_warn_grace_ms", value, sizeof(value))) {
      long v = atol(value);
      this->idle_warn_grace_ms = static_cast<int>((v < 0 ? 0 : (v > 600000 ? 600000 : v)));
    }
    if (kvGet(body, "poweroff_when_idle_ms", value, sizeof(value))) {
      long v = atol(value);
      this->poweroff_when_idle_ms = static_cast<int>((v < 0 ? 0 : (v > 86400000 ? 86400000 : v)));
    }
    if (kvGet(body, "trigger_effect_amplitude", value, sizeof(value))) {
      int v = atoi(value);
      this->trigger_effect_amplitude = v < 0 ? 0 : (v > 8 ? 8 : v);
    }
    if (kvGet(body, "trigger_effect_position", value, sizeof(value))) {
      int v = atoi(value);
      this->trigger_effect_position = v < 0 ? 0 : (v > 9 ? 9 : v);
    }
    if (kvGet(body, "trigger_effect_frequency", value, sizeof(value))) {
      int v = atoi(value);
      this->trigger_effect_frequency = v < 1 ? 1 : (v > 255 ? 255 : v);
    }
    if (kvGet(body, "trigger_effect_end", value, sizeof(value))) {
      int v = atoi(value);
      this->trigger_effect_end = v < 0 ? 0 : (v > 9 ? 9 : v);
    }
    if (kvGet(body, "trigger_effect_period", value, sizeof(value))) {
      int v = atoi(value);
      this->trigger_effect_period = v < 0 ? 0 : (v > 255 ? 255 : v);
    }
    if (kvGet(body, "trigger_effect_amplitude_b", value, sizeof(value))) {
      int v = atoi(value);
      this->trigger_effect_amplitude_b = v < 0 ? 0 : (v > 7 ? 7 : v);
    }
    if (kvGet(body, "trigger_effect_preset", value, sizeof(value))) {
      snprintf(this->trigger_effect_preset, sizeof(this->trigger_effect_preset), "%s",
               value[0] ? value : "custom");
    }
    if (kvGet(body, "trigger_effects", value, sizeof(value)))
      this->trigger_effects = strcmp(value, "true") == 0 || atoi(value) != 0;
    if (kvGet(body, "volume", value, sizeof(value))) setVolume(atoi(value));
    configSave();
    this->volume_dirty = false;
  }

  int libraryText(char *buf, size_t cap) {
    size_t o = 0;
    if (cap) buf[0] = 0;
    for (int i = 0; i < this->count; i++) {
      const Track *s = &this->tracks[i];
      tsvField(buf, cap, &o, s->id, '\t');
      tsvField(buf, cap, &o, s->title, '\t');
      tsvField(buf, cap, &o, s->folder, '\t');
      tsvField(buf, cap, &o, s->path, '\t');
      tsvField(buf, cap, &o, s->ext, '\n');
    }
    return static_cast<int>(o);
  }

  int output(const char *body, char *out, size_t out_cap) {
    char kind[32];
    if (!kvGet(body, "kind", kind, sizeof(kind)))
      return snprintf(out, out_cap, "ok=false\nerror=missing kind\n");
    if (!this->player) return snprintf(out, out_cap, "ok=false\nerror=no player\n");

    char fields[1024];

    if (strcmp(kind, "lightbar") == 0) {
      char rgb[64] = "0,0,0";
      int r = 0, g = 0, b = 0;
      kvGet(body, "rgb", rgb, sizeof(rgb));
      sscanf(rgb, "%d,%d,%d", &r, &g, &b);
      r = r < 0 ? 0 : (r > 255 ? 255 : r);
      g = g < 0 ? 0 : (g > 255 ? 255 : g);
      b = b < 0 ? 0 : (b > 255 ? 255 : b);
      this->led_custom[0] = r;
      this->led_custom[1] = g;
      this->led_custom[2] = b;
      this->led_custom_set = true;
      this->lightbar_on = true;
      this->led_force = true;
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=lightbar %d,%d,%d\n", r, g, b);
    }

    if (strcmp(kind, "lightbar_reset") == 0) {
      this->led_custom_set = false;
      this->lightbar_on = true;
      this->led_force = true;
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=lightbar reset\n");
    }

    if (strcmp(kind, "lightbar_fade") == 0) {
      this->lightbar_on = false;
      this->led_force = true;
      setLightbarSetup(dualsense::OutputBody::LightbarSetup::LightOut);
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=light off\n");
    }

    if (strcmp(kind, "lightbar_setup") == 0) {
      int v = static_cast<int>(kvNumber(body, "value", 0));
      if (v != 1 && v != 2) return snprintf(out, out_cap, "ok=false\nerror=setup must be 1 or 2\n");
      auto setup = static_cast<dualsense::OutputBody::LightbarSetup>(v);
      this->lightbar_on = setup == dualsense::OutputBody::LightbarSetup::LightOn;
      this->led_force = true;
      setLightbarSetup(setup);
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=setup %d\n", v);
    }

    if (strcmp(kind, "lightbar_brightness") == 0) {
      if (this->externalHost()) return snprintf(out, out_cap, "ok=true\nnote=external host\n");
      int v = static_cast<int>(kvNumber(body, "value", 0));
      snprintf(fields, sizeof(fields), "led_brightness=%d", v & 0xFF);
      outputFields(fields);
      return snprintf(out, out_cap, "ok=true\nnote=brightness %d\n", v & 0xFF);
    }

    if (strcmp(kind, "player_leds") == 0) {
      int mask = static_cast<int>(kvNumber(body, "mask", 0)) & 0x3F;
      this->led_manual_player = true;
      this->led_manual_mask = mask;
      serviceLeds();
      return snprintf(out, out_cap, "ok=true\nnote=player leds %d\n", mask);
    }

    if (strcmp(kind, "power_save") == 0) {
      char bits[64] = "";
      kvGet(body, "bits", bits, sizeof(bits));
      int mask = 0;
      char *save = nullptr;
      for (char *tok = strtok_r(bits, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        int bit = atoi(tok);
        if (bit >= 0 && bit < 8) mask |= 1 << bit;
      }
      snprintf(fields, sizeof(fields), "power_save_control=%d", mask);
      outputFields(fields);
      return snprintf(out, out_cap, "ok=true\nnote=power save 0x%02x\n",
                      static_cast<unsigned>(mask));
    }

    if (strcmp(kind, "rumble") == 0) {
      int left = static_cast<int>(kvNumber(body, "left", 0));
      int right = static_cast<int>(kvNumber(body, "right", 0));
      long ms = kvNumber(body, "ms", 0);
      left = left < 0 ? 0 : (left > 255 ? 255 : left);
      right = right < 0 ? 0 : (right > 255 ? 255 : right);
      // The main motors need both ValidFlag0 bits: Rumble (0x01) and Haptics
      // (0x02). DS4Windows and dualsense-tester both set the pair; bit 0 alone
      // is ignored by the controller.
      snprintf(fields, sizeof(fields), "motor_left=%d;motor_right=%d", left, right);
      outputFields(fields);
      this->rumble_left = left;
      this->rumble_right = right;
      this->rumble_until = ms > 0 ? nowMs() + ms : 0;
      return snprintf(out, out_cap, "ok=true\nnote=rumble %d/%d\n", left, right);
    }

    if (strcmp(kind, "trigger") == 0) {
      char side[16] = "right";
      char preset[32] = "";
      char effect[16] = "vibration";
      kvGet(body, "side", side, sizeof(side));
      kvGet(body, "preset", preset, sizeof(preset));
      kvGet(body, "effect", effect, sizeof(effect));
      int position = static_cast<int>(kvNumber(body, "position", 2));
      int amplitude = static_cast<int>(kvNumber(body, "amplitude", 6));
      int frequency = static_cast<int>(kvNumber(body, "frequency", 25));
      int wall = static_cast<int>(kvNumber(body, "wall", 8));
      int start = static_cast<int>(kvNumber(body, "start", position));
      int end = static_cast<int>(kvNumber(body, "end", wall));
      int level = static_cast<int>(kvNumber(body, "level", amplitude));
      int amplitude_b = static_cast<int>(kvNumber(body, "amplitude_b", 0));
      int period = static_cast<int>(kvNumber(body, "period", 0));
      long ms = kvNumber(body, "ms", 0);

      uint8_t params[10] = {};
      int mode = 0x05;
      char label[40] = "off";
      if (preset[0]) {
        const TriggerPreset *found = findTriggerPreset(preset);
        if (!found) return snprintf(out, out_cap, "ok=false\nerror=unknown trigger preset\n");
        mode = encodeTriggerPreset(*found, params);
        snprintf(label, sizeof(label), "%s", found->id);
      } else if (strcmp(effect, "off") == 0 || strcmp(effect, "none") == 0) {
        mode = 0x05;
      } else {
        dualsense::TriggerEffect fx;
        if (strcmp(effect, "feedback") == 0) {
          uint8_t levels[10] = {};
          int last = end < start ? 9 : (end > 9 ? 9 : end);
          for (int zone = start < 0 ? 0 : (start > 9 ? 9 : start); zone <= last; ++zone)
            levels[zone] = static_cast<uint8_t>(level);
          fx.feedback(levels);
        } else if (strcmp(effect, "weapon") == 0) {
          fx.weapon(start, end, static_cast<uint8_t>(level));
        } else if (strcmp(effect, "vibration") == 0) {
          uint8_t amplitudes[10] = {};
          int last = end < start ? 9 : (end > 9 ? 9 : end);
          for (int zone = start < 0 ? 0 : (start > 9 ? 9 : start); zone <= last; ++zone)
            amplitudes[zone] = static_cast<uint8_t>(level);
          fx.vibration(amplitudes, static_cast<uint8_t>(frequency));
        } else if (strcmp(effect, "machine") == 0) {
          fx.machine(start, end, static_cast<uint8_t>(level), static_cast<uint8_t>(amplitude_b),
                     static_cast<uint8_t>(frequency), static_cast<uint8_t>(period));
        } else {
          return snprintf(out, out_cap, "ok=false\nerror=unknown trigger effect\n");
        }
        mode = fx.block[0];
        for (size_t i = 0; i < 10; ++i) params[i] = fx.block[i + 1];
        snprintf(label, sizeof(label), "%s", effect);
      }

      if (mode == 0x05) memset(params, 0, sizeof(params));
      char param_text[96];
      snprintf(param_text, sizeof(param_text), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", params[0],
               params[1], params[2], params[3], params[4], params[5], params[6], params[7],
               params[8], params[9]);

      bool left = strcmp(side, "left") == 0;
      const char *prefix = left ? "left" : "right";
      snprintf(fields, sizeof(fields), "%s_trigger_mode=0x%02x;%s_trigger_params=%s", prefix,
               static_cast<unsigned>(mode), prefix, param_text);
      outputFields(fields);
      this->trigger_side = left ? 1 : 2;
      this->trigger_until = (ms > 0 && mode != 0x05) ? nowMs() + ms : 0;
      return snprintf(out, out_cap, "ok=true\nnote=trigger %s 0x%02x %s\n", side,
                      static_cast<unsigned>(mode), label);
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

  int triggerTest(const char *side, long ms, char *out, size_t cap) {
    if (strcmp(this->trigger_effect_preset, "none") == 0 ||
        strcmp(this->trigger_effect_preset, "off") == 0)
      return snprintf(out, cap, "ok=true\nnote=trigger off\n");
    char request[256];
    snprintf(request, sizeof(request),
             "kind=trigger\nside=%s\neffect=%s\nstart=%d\nend=%d\nlevel=%d\n"
             "amplitude_b=%d\nfrequency=%d\nperiod=%d\nms=%ld",
             side, this->trigger_effect_preset, this->trigger_effect_position,
             this->trigger_effect_end, this->trigger_effect_amplitude,
             this->trigger_effect_amplitude_b, this->trigger_effect_frequency,
             this->trigger_effect_period, ms);
    return output(request, out, cap);
  }

  int debugTrigger(const char *body, char *out, size_t cap) {
    char side[16] = "right";
    char tmp[16];
    if (kvGet(body, "side", tmp, sizeof(tmp))) snprintf(side, sizeof(side), "%s", tmp);
    long ms = kvNumber(body, "ms", 700);
    return triggerTest(side, ms, out, cap);
  }

  int handleCommand(const char *body, size_t len, char *out, size_t cap) {
    char action[64];
    if (!kvGet(body, "action", action, sizeof(action)))
      return snprintf(out, cap, "ok=false\nerror=missing action\n");
    return command(action, body, len, out, cap);
  }

  bool ownsFd(int fd) { return fd >= 0 && (fd == this->player_timer_fd || fd == this->input.fd()); }

private:
  void addTrack(const char *path, const char *folder) {
    if (this->count >= kMaxTracks) return;
    Track *s = &this->tracks[this->count];
    snprintf(s->path, sizeof(s->path), "%s", path);

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
        scanDir(path, e->d_name);
      } else if (S_ISREG(st.st_mode) && audioExt(e->d_name)) {
        addTrack(path, folder);
      }
    }
    closedir(d);
  }

  int findTrack(const char *path) {
    for (int i = 0; i < this->count; i++)
      if (strcmp(this->tracks[i].path, path) == 0) return i;
    return -1;
  }

  int findId(const char *id) {
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
    this->player_timer_fd = this->player.timerFd();
    applyOutputRoute();
    char line[32];
    snprintf(line, sizeof(line), "volume %d\n", this->volume);
    playerSend(line);
    snprintf(line, sizeof(line), "speed %d\n", this->speed);
    playerSend(line);
    if (this->paused) playerSend("pause 1\n");
    this->led_force = true;
    serviceLeds();
  }

  void inputStart() {
    if (this->input.fd() >= 0) return;
    if (!this->player) return;
    if (!this->input.open(this->player.fd(), this->player.node(), this->player.mac())) {
      this->reconnect_at = static_cast<int>((nowMs() + 1500));
      return;
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
    this->input.close();
    if (this->player) this->player.dropDevice();
    this->controller = false;
    if (pause_playback && this->player && this->playing && !this->paused) {
      this->paused = true;
      this->paused_by_disconnect = true;
      playerSend("pause 1\n");
      this->playhead_dirty = true;
    }
    this->input_node[0] = 0;
    this->controller_mac[0] = 0;
    this->usb_data_link = false;
    this->rate = 0;
    this->rate_at = 0;
    this->rate_frames = 0;
    this->led_force = true;
    serviceLeds();
  }

  void playerStatus(const struct player_status &st) {
    pos_ms = st.pos_ms;
    dur_ms = st.dur_ms;
    sent = st.sent;
    underruns = st.underruns;
    paused = st.paused != 0;
    eof = st.eof != 0;
  }

  void playerTrack(const char *path) {
    int idx = findTrack(path);
    if (idx < 0) return;
    if (current >= 0 && current != idx) pushHistory(current);
    current = idx;
    paused = false;
    playing = true;
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

  bool externalHost() const { return this->usb_data_link && !this->player.isUsb(); }

  void applyOutputRoute() {
    if (this->player) this->player.setSpeaker(!this->jack);
  }

  void pauseIfNoJack() {
    if (this->controller && !this->jack && this->playing && !this->paused) {
      this->paused = true;
      this->paused_by_jack = true;
      playerSend("pause 1\n");
      this->playhead_dirty = true;
    }
  }

  void play(int index, long start_ms) {
    if (index < 0 || index >= this->count) return;
    ensureAudibleVolume();
    playerStart();
    char line[kMaxPath + 64];
    snprintf(line, sizeof(line), "fade 1500\nplay %ld %s\n", start_ms, this->tracks[index].path);
    playerSend(line);

    if (this->current >= 0 && this->current != index && this->history_len == 0)
      pushHistory(this->current);
    this->current = index;
    this->playing = true;
    this->paused = false;
    this->eof = false;
    this->pos_ms = start_ms;
    this->dur_ms = 0;
    this->idle_since = nowMs();
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
    if (!this->playing) {
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
      this->paused_by_volume = false;
      this->paused_by_jack = false;
      this->paused_by_disconnect = false;
      this->paused_by_mic = false;
      this->playhead_dirty = true;
    } else {
      this->paused = true;
      this->paused_by_volume = false;
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
      this->volume_dirty = true;
      this->volume_saved_at = nowMs();
    }
    this->volume = v;
    char line[32];
    snprintf(line, sizeof(line), "volume %d\n", v);
    playerSend(line);

    if (v == 0) {
      this->paused_by_disconnect = false;
      if (this->playing && !this->paused) {
        this->paused = true;
        this->paused_by_volume = true;
        playerSend("pause 1\n");
        this->playhead_dirty = true;
      }
    } else if (this->paused_by_volume && this->playing) {
      this->paused = false;
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
    this->pos_ms = ms;
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
        {dualsense::InputBody::Button::Ps, "ps"},
        {dualsense::InputBody::Button::Touchpad, "touchpad"},
        {dualsense::InputBody::Button::Mute, "mute"},
        {dualsense::InputBody::Button::Fn1, "fn1"},
        {dualsense::InputBody::Button::Fn2, "fn2"},
        {dualsense::InputBody::Button::LeftPaddle, "left_paddle"},
        {dualsense::InputBody::Button::RightPaddle, "right_paddle"},
    };

    size_t o = 0;
    out[0] = 0;
    for (auto &n : names) {
      if (!buttons.isOn(n.button)) continue;
      int w = snprintf(out + o, cap - o, "%s%s", o ? "," : "", n.name);
      if (w <= 0 || static_cast<size_t>(w) >= cap - o) break;
      o += static_cast<size_t>(w);
    }
  }

  void inputFrame(const dualsense::InputBody &b, const uint8_t *raw_buf, size_t len) {
    bool was_connected = this->controller;
    this->controller = true;
    if (!was_connected) {
      this->led_force = true;
      this->serviceLeds();
    }
    const dualsense::InputBody::Stick left = b.leftStick();
    const dualsense::InputBody::Stick right = b.rightStick();
    this->lx = left.x;
    this->ly = left.y;
    this->rx = right.x;
    this->ry = right.y;
    const dualsense::InputBody::Triggers triggers = b.triggers();
    this->l2 = triggers.left;
    this->r2 = triggers.right;
    this->battery = b.battery();
    bool had_jack = this->jack;
    bool had_external = this->externalHost();
    this->jack = b.headset();
    this->usb_data_link = b.usbDataLink();
    if (this->externalHost() != had_external) {
      if (this->externalHost()) playerSend("state_clear_lightbar\n");
      this->led_force = true;
      serviceLeds();
    }
    if (!was_connected || had_jack != this->jack) applyOutputRoute();
    if (!was_connected) {
      if (this->paused_by_disconnect) {
        this->paused_by_disconnect = false;
        if (this->jack) {
          this->paused_by_jack = false;
          ensureAudibleVolume();
          if (this->paused) {
            this->paused = false;
            playerSend("pause 0\n");
            this->playhead_dirty = true;
          }
        } else {
          this->paused_by_jack = true;
          pauseIfNoJack();
        }
      } else if (this->jack && this->paused_by_jack) {
        this->paused_by_jack = false;
        ensureAudibleVolume();
        if (this->paused) {
          this->paused = false;
          playerSend("pause 0\n");
          this->playhead_dirty = true;
        }
      } else {
        pauseIfNoJack();
      }
    }
    this->temp = b.temperature();
    snprintf(this->dpad, sizeof(this->dpad), "%s", dpadName(b.dpad()));
    buttonsToString(b.buttons(), this->buttons, sizeof(this->buttons));
    this->raw[0] = 0;
    size_t r = len < (sizeof(this->raw) - 1) / 2 ? len : (sizeof(this->raw) - 1) / 2;
    for (size_t i = 0; i < r; i++)
      snprintf(this->raw + i * 2, sizeof(this->raw) - i * 2, "%02x", raw_buf[i]);
    {
      using ChargingState = dualsense::InputBody::ChargingState;
      ChargingState charge = b.charging();
      snprintf(this->charging, sizeof(this->charging), "%s",
               charge == ChargingState::Charging ? "charging"
               : charge == ChargingState::Full   ? "full"
                                                 : "discharging");
    }
    const dualsense::InputBody::Gyro gyro = b.gyro();
    this->gx = gyro.x;
    this->gy = gyro.y;
    this->gz = gyro.z;
    const dualsense::InputBody::Accelerometer accel = b.accelerometer();
    this->ax = accel.x;
    this->ay = accel.y;
    this->az = accel.z;
    this->seq = static_cast<unsigned>(b.sequence());
    this->tfl = static_cast<unsigned>(b.triggerFeedbackLeft());
    this->tfr = static_cast<unsigned>(b.triggerFeedbackRight());
    for (int t = 0; t < 2; t++) {
      const dualsense::InputBody::Touch touch = b.touch(t);
      this->touch_active[t] = touch.active ? 1 : 0;
      this->touch_id[t] = touch.id;
      this->touch_x[t] = touch.x;
      this->touch_y[t] = touch.y;
    }

    long now = nowMs();
    if (this->rate_at == 0) this->rate_at = now;
    this->rate_frames++;
    if (now - this->rate_at >= 1000) {
      this->rate = static_cast<int>(((this->rate_frames * 1000) / (now - this->rate_at)));
      this->rate_frames = 0;
      this->rate_at = now;
    }
  }

  void inputAction(const char *name, int value) {
    // A USB data host (e.g. a Mac) also receives these reports and applies its
    // own bindings. Keep padwave's UI state updated, but do not act twice.
    // Jack transitions are hardware state used for our own pause/routing.
    if (this->externalHost() && strcmp(name, "jack_plugged") != 0 &&
        strcmp(name, "jack_unplugged") != 0)
      return;
    handleAction(name, value);
  }

  void inputEvent(const char *) { noteActivity(); }

  void inputMic(const int16_t *pcm, size_t samples) {
    if (!this->mic_monitor || samples == 0) return;

    long long sum = 0;
    int peak = 0;
    for (size_t i = 0; i < samples; i++) {
      int v = pcm[i];
      sum += static_cast<long long>(v) * v;
      int a = v < 0 ? -v : v;
      if (a > peak) peak = a;
    }
    int rms = static_cast<int>(sqrt(static_cast<double>(sum) / static_cast<double>(samples)));
    double db = rms > 0 ? 20.0 * log10(static_cast<double>(rms) / 32768.0) : -90.0;
    int level = static_cast<int>(((db + 60.0) * 100.0 / 60.0));
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    this->mic_rms = rms;
    this->mic_peak = peak;
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
    } else if (strcmp(name, "volume") == 0) {
      int before = this->volume;
      setVolume(this->volume + value);
      if (this->trigger_effects && this->volume != before && this->volume > 0) {
        char result[64];
        triggerTest(value < 0 ? "left" : "right", 180, result, sizeof(result));
      }
    } else if (strcmp(name, "seek") == 0) {
      seek(this->pos_ms + static_cast<long>(value) * 1000);
    } else if (strcmp(name, "suspend") == 0 || strcmp(name, "disconnect") == 0) {
      this->playing = false;
      this->paused = true;
      playerSend("pause 1\n");
    } else if (strcmp(name, "jack_unplugged") == 0) {
      applyOutputRoute();
      if (this->playing && !this->paused) {
        this->paused = true;
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

    char line[kMaxPath + 64];
    while (fgets(line, sizeof(line), f)) {
      char *eq = strchr(line, '=');
      if (!eq) continue;
      *eq = 0;
      char *value = eq + 1;
      char *nl = strchr(value, '\n');
      if (nl) *nl = 0;

      if (strcmp(line, "idle_timeout_ms") == 0) {
        long v = atol(value);
        this->idle_timeout_ms = static_cast<int>((v < 1000 ? 1000 : (v > 86400000 ? 86400000 : v)));
      } else if (strcmp(line, "idle_warn_grace_ms") == 0) {
        long v = atol(value);
        this->idle_warn_grace_ms = static_cast<int>((v < 0 ? 0 : (v > 600000 ? 600000 : v)));
      } else if (strcmp(line, "poweroff_when_idle_ms") == 0) {
        long v = atol(value);
        this->poweroff_when_idle_ms = static_cast<int>((v < 0 ? 0 : (v > 86400000 ? 86400000 : v)));
      } else if (strcmp(line, "trigger_effect_amplitude") == 0) {
        int v = atoi(value);
        this->trigger_effect_amplitude = v < 0 ? 0 : (v > 8 ? 8 : v);
      } else if (strcmp(line, "trigger_effect_position") == 0) {
        int v = atoi(value);
        this->trigger_effect_position = v < 0 ? 0 : (v > 9 ? 9 : v);
      } else if (strcmp(line, "trigger_effect_frequency") == 0) {
        int v = atoi(value);
        this->trigger_effect_frequency = v < 1 ? 1 : (v > 255 ? 255 : v);
      } else if (strcmp(line, "trigger_effect_end") == 0) {
        int v = atoi(value);
        this->trigger_effect_end = v < 0 ? 0 : (v > 9 ? 9 : v);
      } else if (strcmp(line, "trigger_effect_period") == 0) {
        int v = atoi(value);
        this->trigger_effect_period = v < 0 ? 0 : (v > 255 ? 255 : v);
      } else if (strcmp(line, "trigger_effect_amplitude_b") == 0) {
        int v = atoi(value);
        this->trigger_effect_amplitude_b = v < 0 ? 0 : (v > 7 ? 7 : v);
      } else if (strcmp(line, "trigger_effect_preset") == 0) {
        snprintf(this->trigger_effect_preset, sizeof(this->trigger_effect_preset), "%s",
                 value[0] ? value : "custom");
      } else if (strcmp(line, "trigger_effects") == 0) {
        this->trigger_effects = strcmp(value, "true") == 0 || atoi(value) != 0;
      } else if (strcmp(line, "volume") == 0) {
        int v = atoi(value);
        this->volume = v < 0 ? 0 : (v > 100 ? 100 : v);
      } else if (strcmp(line, "last_nonzero_volume") == 0) {
        int v = atoi(value);
        this->last_nonzero_volume = v < 0 ? 0 : (v > 100 ? 100 : v);
      } else if (strcmp(line, "speed") == 0) {
        int v = atoi(value);
        this->speed = v < 50 ? 50 : (v > 200 ? 200 : v);
      } else if (strcmp(line, "play_path") == 0) {
        snprintf(this->resume_path, sizeof(this->resume_path), "%s", value);
      } else if (strcmp(line, "play_pos_ms") == 0) {
        this->resume_pos_ms = atol(value);
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
    fprintf(f, "trigger_effect_amplitude=%d\n", this->trigger_effect_amplitude);
    fprintf(f, "trigger_effect_position=%d\n", this->trigger_effect_position);
    fprintf(f, "trigger_effect_frequency=%d\n", this->trigger_effect_frequency);
    fprintf(f, "trigger_effect_end=%d\n", this->trigger_effect_end);
    fprintf(f, "trigger_effect_period=%d\n", this->trigger_effect_period);
    fprintf(f, "trigger_effect_amplitude_b=%d\n", this->trigger_effect_amplitude_b);
    fprintf(f, "trigger_effect_preset=%s\n", this->trigger_effect_preset);
    fprintf(f, "trigger_effects=%s\n", this->trigger_effects ? "true" : "false");
    const char *play_path = this->current >= 0 ? this->tracks[this->current].path : "";
    fputs("play_path=", f);
    for (const char *p = play_path; *p; p++) fputc((*p == '\n' || *p == '\r') ? ' ' : *p, f);
    fputc('\n', f);
    fprintf(f, "play_pos_ms=%ld\n", this->pos_ms);
    fprintf(f, "volume=%d\n", this->volume);
    fprintf(f, "last_nonzero_volume=%d\n", this->last_nonzero_volume);
    fprintf(f, "speed=%d\n", this->speed);
    fclose(f);
    this->playhead_saved_at = nowMs();
  }

  void noteActivity() {
    this->last_activity_ms = nowMs();
    this->idle_since = this->last_activity_ms;
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
    this->idle_warning = false;
    this->suspended = false;
    this->idle_fade_volume = -1;
    this->poweroff_at = 0;
    this->powered_off = false;
  }

  void updateIdle() {
    long now = nowMs();
    if (this->suspended && this->poweroff_at && now >= this->poweroff_at) {
      this->poweroff_at = 0;
      configSave();
      this->powered_off = powerOffController();
    }

    if (!this->playing || this->paused) return;

    long idle_ms = now > this->last_activity_ms ? now - this->last_activity_ms : 0;
    long timeout = this->idle_timeout_ms;
    long grace = this->idle_warn_grace_ms;

    if (!this->idle_warning && !this->suspended && idle_ms >= timeout) {
      this->idle_warning = true;
      this->idle_volume = this->volume;
      this->idle_volume_saved = true;
      this->idle_fade_volume = this->volume;
    }

    if (!this->idle_warning) return;

    long over = idle_ms - timeout;
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
      this->idle_warning = false;
      this->suspended = true;
      this->paused_by_idle = true;
      this->paused = true;
      this->idle_fade_volume = 0;
      playerSend("volume 0\n");
      playerSend("pause 1\n");
      this->playhead_dirty = true;
      if (this->poweroff_when_idle_ms > 0) this->poweroff_at = now + this->poweroff_when_idle_ms;
    }
  }

  bool powerOffController() {
    if (this->player.isUsb()) return false;
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
    if (this->externalHost()) return;
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

  bool applyLightbar(int r, int g, int b, bool force) {
    if (!this->player || this->externalHost()) return false;
    if (!force && this->led_last_rgb[0] == r && this->led_last_rgb[1] == g &&
        this->led_last_rgb[2] == b)
      return true;
    char fields[160];
    snprintf(fields, sizeof(fields), "lightbar_red=%d;lightbar_green=%d;lightbar_blue=%d", r, g, b);
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
    long now = nowMs();
    int state = ledState();

    if (state != this->led_last_state) {
      bool was_off = this->led_last_state == 0;
      bool is_off = state == 0;
      // A custom colour survives device-down/up, but a playback/session state
      // change takes the bar back.
      if (!was_off && !is_off) this->led_custom_set = false;
      this->led_last_state = state;
      if (state == 1) {
        this->led_warning_high = true;
        this->led_warning_at = now;
      }
    }

    if (state == 1 && !this->led_custom_set && now - this->led_warning_at >= 600) {
      this->led_warning_high = !this->led_warning_high;
      this->led_warning_at = now;
    }

    bool charging_now = this->controller && strcmp(this->charging, "charging") == 0;
    if (charging_now && this->lightbar_on && !this->externalHost()) {
      if (this->led_charge_at == 0) {
        // Start the charging loop with setup 2, then 1 two seconds later.
        this->led_charge_at = now;
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
      this->led_charge_at = 0;
      this->led_charge_setup = dualsense::OutputBody::LightbarSetup::LightOut;
      this->led_force = this->lightbar_on;
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

    bool force = this->led_force;
    bool rgb_ok = this->lightbar_on && !charging_now && !this->externalHost() &&
                  applyLightbar(r, g, b, force);
    bool mask_ok = applyPlayerLeds(mask, force);
    if (rgb_ok || mask_ok) this->led_painted = true;
    if (mask_ok) this->led_force = false;
  }

  static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

  static int triggerZone(int pct) {
    int zone = (clampInt(pct, 0, 100) + 5) / 10;
    return zone > 9 ? 9 : zone;
  }

  static int triggerStrength(int pct) {
    if (pct <= 0) return 0;
    int strength = (clampInt(pct, 0, 100) * 8 + 99) / 100;
    return clampInt(strength, 1, 8);
  }

  static int triggerFrequency(int pct) {
    int hz = (clampInt(pct, 0, 100) * 28 + 50) / 100;
    return hz < 1 ? 1 : hz;
  }

  static bool encodeTriggerZones(uint8_t *params, int start, int strength, int frequency) {
    memset(params, 0, 10);
    if (strength <= 0) return false;
    start = clampInt(start, 0, 9);
    uint16_t zones = 0;
    uint32_t packed = 0;
    uint32_t value = static_cast<uint32_t>(((strength - 1) & 7));
    for (int zone = start; zone < 10; zone++) {
      zones |= static_cast<uint16_t>((1u << zone));
      packed |= value << (3 * zone);
    }
    params[0] = static_cast<uint8_t>((zones & 0xFF));
    params[1] = static_cast<uint8_t>((zones >> 8));
    params[2] = static_cast<uint8_t>((packed & 0xFF));
    params[3] = static_cast<uint8_t>(((packed >> 8) & 0xFF));
    params[4] = static_cast<uint8_t>(((packed >> 16) & 0xFF));
    params[5] = static_cast<uint8_t>(((packed >> 24) & 0xFF));
    params[9] = static_cast<uint8_t>(frequency);
    return true;
  }

  static bool encodeTriggerWeapon(uint8_t *params, int start, int end, int strength) {
    memset(params, 0, 10);
    if (strength <= 0) return false;
    start = clampInt(start, 2, 7);
    end = clampInt(end, start + 1, 8);
    uint16_t zones = static_cast<uint16_t>(((1u << start) | (1u << end)));
    params[0] = static_cast<uint8_t>((zones & 0xFF));
    params[1] = static_cast<uint8_t>((zones >> 8));
    params[2] = static_cast<uint8_t>(((strength - 1) & 7));
    return true;
  }

  static const TriggerPreset *findTriggerPreset(const char *id) {
    for (const TriggerPreset &preset : kTriggerPresets)
      if (strcmp(preset.id, id) == 0) return &preset;
    return nullptr;
  }

  static int encodeTriggerPreset(const TriggerPreset &preset, uint8_t *params) {
    int start = triggerZone(preset.start_pct);
    int strength = triggerStrength(preset.force_pct);
    if (strength <= 0) {
      memset(params, 0, 10);
      return 0x05;
    }
    if (preset.mode == 0x25)
      encodeTriggerWeapon(params, start, triggerZone(preset.wall_pct), strength);
    else if (preset.mode == 0x26)
      encodeTriggerZones(params, start, strength, triggerFrequency(preset.wall_pct));
    else
      encodeTriggerZones(params, start, strength, 0);
    return preset.mode;
  }

  void serviceOutput() {
    if (!this->player) return;
    long now = nowMs();
    if (this->rumble_until && now >= this->rumble_until) {
      outputFields("motor_left=0;motor_right=0");
      this->rumble_until = 0;
    }
    if (this->trigger_until && now >= this->trigger_until) {
      char fields[192];
      const char *prefix = this->trigger_side == 1 ? "left" : "right";
      snprintf(fields, sizeof(fields), "%s_trigger_mode=0x05;%s_trigger_params=0,0,0,0,0,0,0,0,0,0",
               prefix, prefix);
      outputFields(fields);
      this->trigger_until = 0;
      this->trigger_side = 0;
    }
  }

  int command(const char *action, const char *body, size_t body_len, char *out, size_t out_cap) {
    (void)body_len;

    bool was_suspended = this->suspended;
    noteActivity();
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
      this->playing = false;
      this->paused = true;
      playerSend("pause 1\n");
    } else if (strcmp(action, "volume") == 0) {
      setVolume(static_cast<int>(kvNumber(body, "value", this->volume)));
    } else if (strcmp(action, "speed") == 0) {
      setSpeed(static_cast<int>(kvNumber(body, "value", this->speed)));
    } else if (strcmp(action, "seek") == 0) {
      seek(this->pos_ms + kvNumber(body, "delta", 0) * 1000);
    } else if (strcmp(action, "seek_to") == 0) {
      seek(kvNumber(body, "position", this->pos_ms / 1000) * 1000);
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
