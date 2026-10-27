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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "padwave_common.hh"
#include "padwave_status.hh"

namespace {

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

struct app_state {
  station stations[kMaxStations];
  int count = 0;
  int current = -1;
  int history[kMaxHistory];
  int history_len = 0;

  int player_in = -1;
  int player_out = -1;
  int player_pid = -1;
  int input_out = -1;
  int input_in = -1;
  int input_pid = -1;

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
  bool player_alive = false;

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
};

app_state g;

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

void add_station(const char *path, const char *game) {
  if (g.count >= kMaxStations) return;
  station *s = &g.stations[g.count];
  snprintf(s->path, sizeof(s->path), "%s", path);

  const char *base = strrchr(path, '/');
  snprintf(s->title, sizeof(s->title), "%s", base ? base + 1 : path);
  char *dot = strrchr(s->title, '.');
  if (dot) *dot = 0;

  snprintf(s->game, sizeof(s->game), "%s", game);
  const char *d = strrchr(path, '.');
  snprintf(s->ext, sizeof(s->ext), "%s", d ? d + 1 : "");
  make_id(path, s->id);
  g.count++;
}

void scan_dir(const char *dir, const char *game) {
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

int find_station(const char *path) {
  for (int i = 0; i < g.count; i++)
    if (strcmp(g.stations[i].path, path) == 0) return i;
  return -1;
}

int find_id(const char *id) {
  for (int i = 0; i < g.count; i++)
    if (strcmp(g.stations[i].id, id) == 0) return i;
  return -1;
}

void push_history(int index) {
  if (index < 0) return;
  if (g.history_len < kMaxHistory) {
    g.history[g.history_len++] = index;
  } else {
    memmove(g.history, g.history + 1, sizeof(int) * (kMaxHistory - 1));
    g.history[kMaxHistory - 1] = index;
  }
}

bool spawn_child(const char *arg1, const char *arg2, int *in_fd, int *out_fd, int *pid_out) {
  int to_child[2], from_child[2];
  if (pipe2(to_child, O_CLOEXEC) != 0) return false;
  if (pipe2(from_child, O_CLOEXEC) != 0) {
    close(to_child[0]);
    close(to_child[1]);
    return false;
  }

  char self[1024];
  ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (n <= 0) {
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    return false;
  }
  self[n] = 0;

  pid_t pid = fork();
  if (pid < 0) {
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    return false;
  }
  if (pid == 0) {
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    if (arg2)
      execl(self, "padwave", arg1, arg2, (char *)nullptr);
    else
      execl(self, "padwave", arg1, (char *)nullptr);
    _exit(127);
  }

  close(to_child[0]);
  close(from_child[1]);
  fcntl(from_child[0], F_SETFL, fcntl(from_child[0], F_GETFL, 0) | O_NONBLOCK);
  *in_fd = to_child[1];
  *out_fd = from_child[0];
  *pid_out = (int)pid;
  return true;
}

void player_send(const char *line) {
  if (g.player_in < 0) return;
  size_t len = strlen(line);
  size_t put = 0;
  while (put < len) {
    ssize_t n = write(g.player_in, line + put, len - put);
    if (n <= 0) return;
    put += (size_t)n;
  }
}

void player_start() {
  if (g.player_alive) return;
  if (!spawn_child("native", nullptr, &g.player_in, &g.player_out, &g.player_pid)) return;
  g.player_alive = true;
}

void input_start() {
  if (g.input_out >= 0) return;
  char node[64];
  if (find_dualsense(node, sizeof(node)) < 0) {
    g.reconnect_at = (int)(now_ms() + 1500);
    return;
  }
  if (!spawn_child("input", node, &g.input_in, &g.input_out, &g.input_pid)) return;
}

void input_stop() {
  if (g.input_in >= 0) close(g.input_in);
  if (g.input_out >= 0) close(g.input_out);
  g.input_in = g.input_out = g.input_pid = -1;
  g.controller = false;
}

void input_line(const char *line);

void player_line(const char *line) {
  if (strncmp(line, "pos ", 4) == 0) {
    g.pos_ms = atol(line + 4);
  } else if (strncmp(line, "dur ", 4) == 0) {
    g.dur_ms = atol(line + 4);
  } else if (strncmp(line, "sent ", 5) == 0) {
    g.sent = atol(line + 5);
  } else if (strncmp(line, "underruns ", 10) == 0) {
    g.underruns = atol(line + 10);
  } else if (strncmp(line, "track ", 6) == 0) {
    int idx = find_station(line + 6);
    if (idx >= 0) {
      if (g.current >= 0 && g.current != idx) push_history(g.current);
      g.current = idx;
      g.paused = false;
      g.playing = true;
    }
  } else if (strcmp(line, "eof") == 0) {
    g.eof = true;
    g.playing = false;
  }
}

void app_play(int index, long start_ms) {
  if (index < 0 || index >= g.count) return;
  player_start();
  char line[kMaxPath + 64];
  snprintf(line, sizeof(line), "fade 1500\nplay %ld %s\n", start_ms, g.stations[index].path);
  player_send(line);

  if (g.current >= 0 && g.current != index && g.history_len == 0) push_history(g.current);
  g.current = index;
  g.playing = true;
  g.paused = false;
  g.eof = false;
  g.pos_ms = start_ms;
  g.dur_ms = 0;
  g.idle_since = now_ms();

  // Gapless: queue a different station for the junction.
  if (g.count > 1) {
    int next = rand() % g.count;
    if (next == index) next = (next + 1) % g.count;
    char q[kMaxPath + 16];
    snprintf(q, sizeof(q), "queue %s\n", g.stations[next].path);
    player_send(q);
  }
}

void app_next() {
  if (g.count == 0) return;
  int next = rand() % g.count;
  if (g.count > 1 && next == g.current) next = (next + 1) % g.count;
  app_play(next, 0);
}

void app_previous() {
  if (g.history_len > 0) {
    int prev = g.history[--g.history_len];
    app_play(prev, 0);
  } else if (g.current > 0) {
    app_play(g.current - 1, 0);
  }
}

void app_shuffle_game() {
  if (g.count == 0) return;
  const char *game = g.current >= 0 ? g.stations[g.current].game : "";
  for (int tries = 0; tries < 100; tries++) {
    int i = rand() % g.count;
    if (strcmp(g.stations[i].game, game) != 0) {
      app_play(i, 0);
      return;
    }
  }
  app_next();
}

void app_toggle_pause() {
  if (!g.playing) {
    if (g.current >= 0)
      app_play(g.current, g.pos_ms);
    return;
  }
  g.paused = !g.paused;
  player_send(g.paused ? "pause 1\n" : "pause 0\n");
}

void app_set_volume(int v) {
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  g.volume = v;
  g.muted = v == 0;
  char line[32];
  snprintf(line, sizeof(line), "volume %d\n", v);
  player_send(line);
}

void app_seek(long ms) {
  if (ms < 0) ms = 0;
  if (g.dur_ms > 0 && ms > g.dur_ms) ms = g.dur_ms;
  char line[32];
  snprintf(line, sizeof(line), "seek %ld\n", ms);
  player_send(line);
  g.pos_ms = ms;
}

void controller_from_frame(const char *line) {
  // frame seq=.. lx=.. ... dpad=.. buttons=.. battery=.. charging=.. jack=..
  g.controller = true;
  char buttons[256] = "";
  char dpad[16] = "none";
  const char *p = line;
  while ((p = strchr(p, ' ')) != nullptr) {
    p++;
    if (strncmp(p, "lx=", 3) == 0) g.lx = atoi(p + 3);
    else if (strncmp(p, "ly=", 3) == 0) g.ly = atoi(p + 3);
    else if (strncmp(p, "rx=", 3) == 0) g.rx = atoi(p + 3);
    else if (strncmp(p, "ry=", 3) == 0) g.ry = atoi(p + 3);
    else if (strncmp(p, "l2=", 3) == 0) g.l2 = atoi(p + 3);
    else if (strncmp(p, "r2=", 3) == 0) g.r2 = atoi(p + 3);
    else if (strncmp(p, "battery=", 8) == 0) g.battery = atoi(p + 8);
    else if (strncmp(p, "charging=", 9) == 0) {
      int c = atoi(p + 9);
      snprintf(g.charging, sizeof(g.charging), "%s",
               c == 1 ? "charging" : c == 2 ? "full" : "discharging");
    } else if (strncmp(p, "jack=", 5) == 0) g.jack = atoi(p + 5) != 0;
    else if (strncmp(p, "temp=", 5) == 0) g.temp = atoi(p + 5);
    else if (strncmp(p, "ax=", 3) == 0) g.ax = atoi(p + 3);
    else if (strncmp(p, "ay=", 3) == 0) g.ay = atoi(p + 3);
    else if (strncmp(p, "az=", 3) == 0) g.az = atoi(p + 3);
    else if (strncmp(p, "gx=", 3) == 0) g.gx = atoi(p + 3);
    else if (strncmp(p, "gy=", 3) == 0) g.gy = atoi(p + 3);
    else if (strncmp(p, "gz=", 3) == 0) g.gz = atoi(p + 3);
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
      if (n >= sizeof(g.raw)) n = sizeof(g.raw) - 1;
      memcpy(g.raw, p + 4, n);
      g.raw[n] = 0;
    }
  }
  snprintf(g.dpad, sizeof(g.dpad), "%s", dpad);
  snprintf(g.buttons, sizeof(g.buttons), "%s", buttons);
}

void app_action(const char *action) {
  if (strcmp(action, "next") == 0) app_next();
  else if (strcmp(action, "previous") == 0) app_previous();
  else if (strcmp(action, "shuffle_game") == 0) app_shuffle_game();
  else if (strcmp(action, "pause") == 0) app_toggle_pause();
  else if (strcmp(action, "mute") == 0) app_set_volume(g.muted ? g.volume : 0);
  else if (strcmp(action, "disconnect") == 0) {
    g.playing = false;
    g.paused = true;
    player_send("pause 1\n");
  }
  snprintf(g.last_action, sizeof(g.last_action), "%s", action);
}

void input_line(const char *line) {
  if (strncmp(line, "frame ", 6) == 0) {
    controller_from_frame(line);
  } else if (strncmp(line, "action ", 7) == 0) {
    char action[64];
    snprintf(action, sizeof(action), "%s", line + 7);
    char *space = strchr(action, ' ');
    if (space && strncmp(action, "volume ", 7) == 0) {
      int delta = atoi(space + 1);
      app_set_volume(g.volume + delta);
      snprintf(g.last_action, sizeof(g.last_action), "volume");
    } else if (space && strncmp(action, "seek ", 5) == 0) {
      app_seek(g.pos_ms + atol(space + 1) * 1000);
      snprintf(g.last_action, sizeof(g.last_action), "seek");
    } else {
      if (space) *space = 0;
      app_action(action);
    }
  } else if (strncmp(line, "down", 4) == 0) {
    input_stop();
    g.reconnect_at = (int)(now_ms() + 1500);
  }
}

void drain_lines(int fd, void (*handler)(const char *)) {
  char buf[8192];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    char *start = buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != nullptr) {
      *nl = 0;
      handler(start);
      start = nl + 1;
    }
  }
}

void status_radio(char *buf, size_t cap) {
  const station *s = g.current >= 0 ? &g.stations[g.current] : nullptr;
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
           g.playing ? 1 : 0, g.paused ? 1 : 0, g.pos_ms / 1000.0, g.dur_ms / 1000.0, g.volume,
           g.muted ? 1 : 0, g.volume, cur);
}

void status_controller(char *buf, size_t cap) {
  char buttons[512];
  json_escape(g.buttons, buttons, sizeof(buttons));
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
  json_escape(g.raw, raw, sizeof(raw));

  snprintf(buf, cap,
           "{\"connected\":%d,\"bus\":\"bluetooth\",\"name\":\"DualSense\",\"mac\":\"\","
           "\"model\":\"dualsense\",\"node\":\"%s\",\"battery\":%d,\"charging\":\"%s\","
           "\"jack\":%d,\"buttons\":%s,\"dpad\":\"%s\",\"l2\":%d,\"r2\":%d,"
           "\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"temperature\":%d,"
           "\"accel\":[%d,%d,%d],\"gyro\":[%d,%d,%d],\"raw\":\"%s\","
           "\"last_action\":\"%s\",\"fields\":{},\"touches\":[]}",
           g.controller ? 1 : 0, idx >= 0 ? node : "", g.battery >= 0 ? g.battery : 0, g.charging, g.jack ? 1 : 0, arr, g.dpad,
           g.l2, g.r2, g.lx, g.ly, g.rx, g.ry, g.temp, g.ax, g.ay, g.az, g.gx, g.gy, g.gz, raw,
           g.last_action);
}

} // namespace

int app_status_json(char *buf, size_t cap) {
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
                  radio, g.player_alive ? 1 : 0, g.pos_ms / 1000.0, g.dur_ms / 1000.0,
                  g.paused ? 1 : 0, g.volume, g.sent, g.underruns, g.eof ? 1 : 0, controller,
                  (long)(time(nullptr)));
}

int app_library_json(char *buf, size_t cap) {
  int n = snprintf(buf, cap, "{\"tracks\":[");
  for (int i = 0; i < g.count && n > 0 && (size_t)n < cap; i++) {
    const station *s = &g.stations[i];
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

int app_command(const char *action, const char *body, size_t body_len, char *out, size_t out_cap) {
  (void)body_len;

  if (strcmp(action, "next") == 0) app_next();
  else if (strcmp(action, "previous") == 0) app_previous();
  else if (strcmp(action, "shuffle_game") == 0) app_shuffle_game();
  else if (strcmp(action, "pause") == 0) app_toggle_pause();
  else if (strcmp(action, "mute") == 0) app_set_volume(g.muted ? g.volume : 0);
  else if (strcmp(action, "disconnect") == 0) {
    g.playing = false;
    g.paused = true;
    player_send("pause 1\n");
  } else if (strcmp(action, "volume") == 0) {
    app_set_volume((int)json_number(body, "value", g.volume));
  } else if (strcmp(action, "seek") == 0) {
    app_seek(g.pos_ms + json_number(body, "delta", 0) * 1000);
  } else if (strcmp(action, "seek_to") == 0) {
    app_seek(json_number(body, "position", g.pos_ms / 1000) * 1000);
  } else if (strcmp(action, "play_track") == 0) {
    char id[64];
    if (json_string(body, "id", id, sizeof(id))) {
      int idx = find_id(id);
      if (idx >= 0) app_play(idx, 0);
    }
  } else {
    return snprintf(out, out_cap, "{\"ok\":false,\"error\":\"unknown action\"}");
  }

  snprintf(g.last_action, sizeof(g.last_action), "%s", action);
  return snprintf(out, out_cap, "{\"ok\":true}");
}

int app_handle_command(const char *body, size_t len, char *out, size_t cap) {
  char action[64];
  if (!json_string(body, "action", action, sizeof(action)))
    return snprintf(out, cap, "{\"ok\":false,\"error\":\"missing action\"}");
  return app_command(action, body, len, out, cap);
}

bool app_owns_fd(int fd) {
  return (fd >= 0 && (fd == g.player_out || fd == g.input_out));
}

bool app_init() {
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

  qsort(g.stations, (size_t)g.count, sizeof(station), cmp_station);
  srand((unsigned)time(nullptr) ^ (unsigned)getpid());
  g.player_alive = false;
  g.player_in = g.player_out = -1;
  g.input_in = g.input_out = -1;
  app_set_volume(70);
  return true;
}

void app_shutdown() {
  if (g.player_in >= 0) {
    player_send("quit\n");
    close(g.player_in);
    close(g.player_out);
    g.player_in = g.player_out = -1;
  }
  input_stop();
}

void app_tick() {
  if (g.eof) {
    g.eof = false;
    app_next();
  }
  if (!g.player_alive) {
    player_start();
    if (g.player_alive && g.current < 0 && g.count > 0) {
      int start = rand() % g.count;
      app_play(start, 0);
    }
  }
  if (g.input_out < 0 && (int)now_ms() >= g.reconnect_at) input_start();
}

int app_fds(int *fds, int max) {
  int n = 0;
  if (g.player_out >= 0 && n < max) fds[n++] = g.player_out;
  if (g.input_out >= 0 && n < max) fds[n++] = g.input_out;
  return n;
}

void app_handle_fd(int fd) {
  if (fd == g.player_out) drain_lines(fd, player_line);
  else if (fd == g.input_out) drain_lines(fd, input_line);
}
