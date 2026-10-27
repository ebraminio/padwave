// padwave_input.hh — the controller input engine.
//
// The decoder is an engine the app owns: input_create() opens the hidraw node,
// input_set_sink() installs four callbacks (frame, action, event, mic) plus
// userdata, and input_feed() hands it bytes. The decoder keeps its edge state
// in the handle. The same code backs the `padwave input` CLI, which installs no
// callbacks.
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "padwave_wire.hh"

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
