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
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

#include "padwave_common.hh"
#include "padwave_schemas.hh"
#include "padwave_status.hh"

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

volatile sig_atomic_t g_running = 1;

void on_signal(int) { g_running = 0; }

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
int64_t now_ms();
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
    g_actions.ps_at = now_ms();
  }

  if (!(buttons & padwave::InputBody::Ps)) {
    g_actions.ps_armed = false;
  } else if (g_actions.ps_armed && now_ms() - g_actions.ps_at >= 2000) {
    emit_action("suspend");
    g_actions.ps_armed = false;
  }
}

void track_triggers(int l2, int r2) {
  int64_t now = now_ms();
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
  int64_t now = now_ms();
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

int64_t now_ms() {
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
  int64_t now = now_ms();

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
  int64_t now = now_ms();
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

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  void *h = input_create(argc > 1 ? argv[1] : nullptr);
  if (!h) return 1;
  printf("ready\n");

  int fd = input_fd(h);
  while (g_running) {
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
