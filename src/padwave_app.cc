// The standalone padwave app: it owns the player and input children and the
// station state the panel reads. Deliberately plain C++ (no STL, no
// exceptions), like the rest of the native side.
#include "padwave_app.hh"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "padwave_common.hh"
#include "padwave_modes.hh"
#include "padwave_status.hh"


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
bool app_owns_fd(app *a, int fd) { return a->owns_fd(fd); }
