// padwave_input.hh — the controller input engine.
//
// One Input object owns the hidraw fd, the four callbacks, the edge detectors
// and the decoder. The app embeds one; there is no separate input process or
// CLI. Fields and helpers are private; the public surface is open/close/fd/
// node/setSink/feed.
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "padwave_hid.hh"
#include "padwave_wire.hh"

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

// padwave input — the controller's state, decoded by the C++ schema.
// The engine is in-process; the app owns one Input and installs callbacks.

struct Input {
private:
  struct ActionsState {
    uint32_t prev_buttons = 0;
    int64_t last_l2 = 0;
    int64_t last_r2 = 0;
    int64_t ps_at = 0;
    bool ps_armed = false;
  };

  struct SensorsState {
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

  struct DpadState {
    uint8_t prev = 0xFF;
    int hold_dir = 0;
    int hold_step = 0;
    int64_t hold_at = 0;
  };

  struct Binding {
    uint32_t bit;
    const char *action;
  };

  int fd_ = -1;
  char node_name[64] = "";
  char mac_[32] = "";
  input_frame_fn frame_fn = nullptr;
  input_action_fn action_fn = nullptr;
  input_event_fn event_fn = nullptr;
  input_mic_fn mic_fn = nullptr;
  void *ud = nullptr;

  ActionsState actions;
  SensorsState sensors;
  DpadState dpad_state;

  static constexpr Binding kBindings[] = {
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

  void trackButtons(uint32_t buttons) {
    uint32_t pressed = buttons & ~actions.prev_buttons;
    actions.prev_buttons = buttons;

    for (auto &b : kBindings)
      if (pressed & b.bit) emitAction(b.action, 0);

    if (pressed & padwave::InputBody::Ps) {
      actions.ps_armed = true;
      actions.ps_at = nowMs();
    }

    if (!(buttons & padwave::InputBody::Ps)) {
      actions.ps_armed = false;
    } else if (actions.ps_armed && nowMs() - actions.ps_at >= 2000) {
      emitAction("suspend", 0);
      actions.ps_armed = false;
    }
  }

  void trackTriggers(int l2, int r2) {
    int64_t now = nowMs();
    if (l2 >= 30 && now - actions.last_l2 >= 110) {
      int step = l2 >= 220 ? 4 : (l2 >= 150 ? 2 : 1);
      emitAction("volume", -step);
      actions.last_l2 = now;
    }
    if (r2 >= 30 && now - actions.last_r2 >= 110) {
      int step = r2 >= 220 ? 4 : (r2 >= 150 ? 2 : 1);
      emitAction("volume", step);
      actions.last_r2 = now;
    }
  }

  void trackSensors(const padwave::InputBody &b) {
    int64_t now = nowMs();
    bool jack = (b.status1.get() & padwave::InputBody::Headset) != 0;
    int l2 = b.l2.get(), r2 = b.r2.get();
    int lx = b.lx.get(), ly = b.ly.get(), rx = b.rx.get(), ry = b.ry.get();
    uint32_t buttons = b.buttons;
    uint8_t dpad = (uint8_t)(buttons & 0x0F);
    bool first = !sensors.has_prev;

    bool real = false, analog = false;
    if (!first) {
      real = buttons != sensors.prev_buttons || dpad != sensors.prev_dpad ||
             jack != sensors.prev_jack || abs(l2 - sensors.prev_l2) > 10 ||
             abs(r2 - sensors.prev_r2) > 10;
      analog = abs(lx - sensors.prev_lx) > 24 || abs(ly - sensors.prev_ly) > 24 ||
               abs(rx - sensors.prev_rx) > 24 || abs(ry - sensors.prev_ry) > 24;
    }

    double g =
        sqrt((double)b.accel_x.get() * b.accel_x.get() + (double)b.accel_y.get() * b.accel_y.get() +
             (double)b.accel_z.get() * b.accel_z.get()) /
        8192.0;
    if (g >= 2.25 && now - sensors.last_shake > 1500) {
      emitEvent("shake");
      sensors.last_shake = now;
      real = true;
    }

    if (real || analog) {
      bool capped = analog && !real && sensors.analog_resets >= 6;
      if (!capped && now - sensors.last_activity > 900) {
        emitEvent("activity");
        sensors.last_activity = now;
        if (analog)
          sensors.analog_resets++;
        else
          sensors.analog_resets = 0;
      } else if (real) {
        sensors.analog_resets = 0;
      }
    }

    if (first) {
      sensors.stable_jack = jack ? 1 : 0;
    } else if (jack == (sensors.stable_jack != 0)) {
      sensors.pending_jack = -1;
      sensors.jack_since = 0;
    } else if (sensors.pending_jack != (jack ? 1 : 0) || sensors.jack_since == 0) {
      sensors.pending_jack = jack ? 1 : 0;
      sensors.jack_since = now;
    } else if (now - sensors.jack_since >= 250) {
      emitAction(jack ? "jack_plugged" : "jack_unplugged", 0);
      sensors.stable_jack = jack ? 1 : 0;
      sensors.pending_jack = -1;
      sensors.jack_since = 0;
    }

    if (!sensors.l2_down && l2 >= 30) {
      emitEvent("trigger_l_pressed");
      sensors.l2_down = true;
    } else if (sensors.l2_down && l2 < 15) {
      emitEvent("trigger_l_released");
      sensors.l2_down = false;
    }
    if (!sensors.r2_down && r2 >= 30) {
      emitEvent("trigger_r_pressed");
      sensors.r2_down = true;
    } else if (sensors.r2_down && r2 < 15) {
      emitEvent("trigger_r_released");
      sensors.r2_down = false;
    }

    sensors.has_prev = true;
    sensors.prev_buttons = buttons;
    sensors.prev_dpad = dpad;
    sensors.prev_lx = lx;
    sensors.prev_ly = ly;
    sensors.prev_rx = rx;
    sensors.prev_ry = ry;
    sensors.prev_l2 = l2;
    sensors.prev_r2 = r2;
    sensors.prev_jack = jack;
  }

  void trackDpad(uint32_t buttons) {
    static const int steps[] = {10, 20, 40, 60};
    static const int intervals[] = {320, 260, 190, 130};

    uint8_t dpad = (uint8_t)(buttons & 0x0F);
    int64_t now = nowMs();

    if (dpad != dpad_state.prev) {
      dpad_state.prev = dpad;
      dpad_state.hold_dir = 0;

      switch (dpad) {
      case padwave::InputBody::Left:
        emitAction("seek", -6);
        dpad_state.hold_dir = -1;
        dpad_state.hold_step = 0;
        dpad_state.hold_at = now + 420;
        break;
      case padwave::InputBody::Right:
        emitAction("seek", 6);
        dpad_state.hold_dir = 1;
        dpad_state.hold_step = 0;
        dpad_state.hold_at = now + 420;
        break;
      case padwave::InputBody::Up: emitAction("next_random", 0); break;
      case padwave::InputBody::Down: emitAction("previous", 0); break;
      default: break;
      }
      return;
    }

    if (dpad_state.hold_dir == 0 || now < dpad_state.hold_at) return;

    int index = dpad_state.hold_step < 4 ? dpad_state.hold_step : 3;
    emitAction("seek", dpad_state.hold_dir * steps[index]);
    dpad_state.hold_step = index + 1;
    dpad_state.hold_at = now + intervals[index];
  }

  static int64_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  }

  void emitAction(const char *name, int value) {
    if (action_fn) action_fn(name, value, ud);
  }

  void emitEvent(const char *name) {
    if (event_fn) event_fn(name, ud);
  }

  static const char *dpadName(uint8_t value) {
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

  static void buttonsToString(uint32_t buttons, char *out, size_t cap) {
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

  void emitFrame(const padwave::InputBody &b, const uint8_t *raw, size_t len) {
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
    snprintf(f.dpad, sizeof(f.dpad), "%s", dpadName(buttons & 0x0F));
    buttonsToString(buttons, f.buttons, sizeof(f.buttons));
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

    if (frame_fn) frame_fn(&f, ud);

    trackDpad(b.buttons);
    trackButtons(b.buttons);
    trackTriggers(b.l2, b.r2);
    trackSensors(b);
  }

  void handleReport(const uint8_t *buf, size_t len) {
    if (!buf || len == 0) return;

    if (len > 7 && padwave::MicFrame::is_frame(buf, len)) {
      size_t payload_len = 0;
      const uint8_t *payload = padwave::MicFrame::payload(buf, len, &payload_len);
      if (mic_fn) mic_fn(payload, payload_len, ud);
      return;
    }

    if (buf[0] == padwave::PacketStateBluetooth && len >= padwave::InputReport::Size) {
      const padwave::InputReport *r = (const padwave::InputReport *)buf;
      if (!r->valid()) return;
      emitFrame(r->body, buf, padwave::InputReport::Size);
      return;
    }

    if (buf[0] == padwave::PacketInputUsb && len >= padwave::InputReport::UsbSize)
      emitFrame(*(const padwave::InputBody *)(buf + 1), buf, padwave::InputReport::UsbSize);
  }

public:
  Input() = default;

  ~Input() { close(); }

  bool open(const char *node = nullptr) {
    close();
    const char *path = node;
    if (!node) {
      DualsenseDevice device;
      if (!device) {
        fprintf(stderr, "padwave: no DualSense hidraw node\n");
        return false;
      }
      path = device.node;
      snprintf(mac_, sizeof(mac_), "%s", device.mac);
    }
    fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
      fprintf(stderr, "padwave: cannot open %s\n", path);
      return false;
    }
    snprintf(node_name, sizeof(node_name), "%s", path);
    return true;
  }

  void close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    node_name[0] = 0;
    mac_[0] = 0;
    frame_fn = nullptr;
    action_fn = nullptr;
    event_fn = nullptr;
    mic_fn = nullptr;
    ud = nullptr;
    actions = ActionsState{};
    sensors = SensorsState{};
    dpad_state = DpadState{};
  }

  int fd() const { return fd_; }

  const char *node() const { return node_name; }

  const char *mac() const { return mac_; }

  void setSink(input_frame_fn frame, input_action_fn action, input_event_fn event, input_mic_fn mic,
               void *userdata) {
    frame_fn = frame;
    action_fn = action;
    event_fn = event;
    mic_fn = mic;
    ud = userdata;
  }

  int feed(const uint8_t *buf, size_t len) {
    if (fd_ < 0) return -1;
    handleReport(buf, len);
    return 0;
  }

  // Pure parser for tests and fuzzing: no fd required, no callbacks needed.
  int parse(const uint8_t *buf, size_t len) {
    handleReport(buf, len);
    return 0;
  }
};
