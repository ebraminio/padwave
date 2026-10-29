// padwave_input.hh — the controller input engine.
//
// One Input object borrows the player's hidraw fd, and owns the App pointer,
// the edge detectors and the decoder. The app embeds one; there is no separate
// input process or CLI. Fields and helpers are private; the public surface is
// open/close/fd/node/attach/feed.
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

#include "padwave_wire.hh"

// padwave input — the controller's state, decoded by the C++ schema.
// The engine is in-process; the app owns one Input and attaches itself.

struct App;

struct Input {
private:
  struct ActionsState {
    padwave::InputBody::Buttons prevButtons = padwave::InputBody::Buttons();
    int64_t last_l2 = 0;
    int64_t last_r2 = 0;
    int64_t ps_at = 0;
    bool ps_armed = false;
  };

  struct SensorsState {
    bool has_prev = false;
    padwave::InputBody::Buttons prevButtons = padwave::InputBody::Buttons();
    padwave::InputBody::Dpad prevDpad = padwave::InputBody::Dpad::None;
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
    padwave::InputBody::Dpad prev = padwave::InputBody::Dpad::None;
    int hold_dir = 0;
    int hold_step = 0;
    int64_t hold_at = 0;
  };

  struct Binding {
    padwave::InputBody::Button button;
    const char *action;
  };

  int fd_ = -1;
  char node_name[64] = "";
  char mac_[32] = "";
  App *app = nullptr;

  ActionsState actions;
  SensorsState sensors;
  DpadState dpad_state;

  static constexpr Binding kBindings[] = {
      {padwave::InputBody::Button::ButtonTriangle, "toggle_pause"},
      {padwave::InputBody::Button::ButtonCircle, "play"},
      {padwave::InputBody::Button::ButtonSquare, "shuffle_folder"},
      {padwave::InputBody::Button::L1, "previous"},
      {padwave::InputBody::Button::R1, "next"},
      {padwave::InputBody::Button::Create, "previous"},
      {padwave::InputBody::Button::Options, "next"},
      {padwave::InputBody::Button::Touchpad, "toggle_mute"},
      {padwave::InputBody::Button::Mute, "toggle_mute"},
      {padwave::InputBody::Button::L3, "next"},
      {padwave::InputBody::Button::R3, "previous"},
      {padwave::InputBody::Button::LeftPaddle, "previous"},
      {padwave::InputBody::Button::RightPaddle, "next"},
      {padwave::InputBody::Button::Fn1, "toggle_pause"},
      {padwave::InputBody::Button::Fn2, "toggle_mute"},
  };

  void trackButtons(padwave::InputBody::Buttons buttons) {
    padwave::InputBody::Buttons pressed = buttons.pressed(actions.prevButtons);
    actions.prevButtons = buttons;

    for (auto &b : kBindings)
      if (pressed.isOn(b.button)) emitAction(b.action, 0);

    if (pressed.isOn(padwave::InputBody::Button::Ps)) {
      actions.ps_armed = true;
      actions.ps_at = nowMs();
    }

    if (!buttons.isOn(padwave::InputBody::Button::Ps)) {
      actions.ps_armed = false;
    } else if (actions.ps_armed && nowMs() - actions.ps_at >= padwave::InputBody::PsHoldMs) {
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
    bool jack = b.headset();
    const auto sticks = b.sticks();
    int l2 = sticks.l2, r2 = sticks.r2;
    int lx = sticks.lx, ly = sticks.ly, rx = sticks.rx, ry = sticks.ry;
    const padwave::InputBody::Buttons buttons = b.buttons();
    padwave::InputBody::Dpad dpad = b.dpad();
    bool first = !sensors.has_prev;

    bool real = false, analog = false;
    if (!first) {
      real = buttons != sensors.prevButtons || dpad != sensors.prevDpad ||
             jack != sensors.prev_jack || abs(l2 - sensors.prev_l2) > 10 ||
             abs(r2 - sensors.prev_r2) > 10;
      analog = abs(lx - sensors.prev_lx) > 24 || abs(ly - sensors.prev_ly) > 24 ||
               abs(rx - sensors.prev_rx) > 24 || abs(ry - sensors.prev_ry) > 24;
    }

    float g = b.accelerometer().len();
    if (g >= 2.25f && now - sensors.last_shake > 1500) {
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
    sensors.prevButtons = buttons;
    sensors.prevDpad = dpad;
    sensors.prev_lx = lx;
    sensors.prev_ly = ly;
    sensors.prev_rx = rx;
    sensors.prev_ry = ry;
    sensors.prev_l2 = l2;
    sensors.prev_r2 = r2;
    sensors.prev_jack = jack;
  }

  void trackDpad(padwave::InputBody::Dpad dpad) {
    static const int steps[] = {10, 20, 40, 60};
    static const int intervals[] = {320, 260, 190, 130};

    int64_t now = nowMs();

    if (dpad != dpad_state.prev) {
      dpad_state.prev = dpad;
      dpad_state.hold_dir = 0;

      if (dpad == padwave::InputBody::Dpad::Left) {
        emitAction("seek", -6);
        dpad_state.hold_dir = -1;
        dpad_state.hold_step = 0;
        dpad_state.hold_at = now + 420;
      } else if (dpad == padwave::InputBody::Dpad::Right) {
        emitAction("seek", 6);
        dpad_state.hold_dir = 1;
        dpad_state.hold_step = 0;
        dpad_state.hold_at = now + 420;
      } else if (dpad == padwave::InputBody::Dpad::Up) {
        emitAction("next", 0);
      } else if (dpad == padwave::InputBody::Dpad::Down) {
        emitAction("previous", 0);
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
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
  }

  void emitAction(const char *name, int value);
  void emitEvent(const char *name);
  void emitMic(const uint8_t *data, size_t len);
  void emitFrameToApp(const padwave::InputBody &b, const uint8_t *raw, size_t len);

  void emitFrame(const padwave::InputBody &b, const uint8_t *raw, size_t len) {
    emitFrameToApp(b, raw, len);
    trackDpad(b.dpad());
    trackButtons(b.buttons());
    const auto sticks = b.sticks();
    trackTriggers(sticks.l2, sticks.r2);
    trackSensors(b);
  }

  void handleReport(const uint8_t *buf, size_t len) {
    if (!buf || len == 0) return;

    if (len > 7 && padwave::MicFrame::is_frame(buf, len)) {
      size_t payload_len = 0;
      const uint8_t *payload = padwave::MicFrame::payload(buf, len, &payload_len);
      emitMic(payload, payload_len);
      return;
    }

    if (buf[0] == padwave::PacketStateBluetooth && len >= padwave::InputReport::Size) {
      const padwave::InputReport *r = reinterpret_cast<const padwave::InputReport *>(buf);
      if (!r->valid()) return;
      emitFrame(r->body, buf, padwave::InputReport::Size);
      return;
    }

    if (buf[0] == padwave::PacketInputUsb && len >= padwave::InputReport::UsbSize)
      emitFrame(*reinterpret_cast<const padwave::InputBody *>((buf + 1)), buf,
                padwave::InputReport::UsbSize);
  }

public:
  Input() = default;

  ~Input() { close(); }

  // Input borrows the player's O_RDWR fd; it never opens or closes the device.
  bool open(int fd, const char *node, const char *mac) {
    close();
    if (fd < 0) return false;
    fd_ = fd;
    snprintf(node_name, sizeof(node_name), "%s", node ? node : "");
    snprintf(mac_, sizeof(mac_), "%s", mac ? mac : "");
    return true;
  }

  void close() {
    fd_ = -1;
    node_name[0] = 0;
    mac_[0] = 0;
    app = nullptr;
    actions = ActionsState{};
    sensors = SensorsState{};
    dpad_state = DpadState{};
  }

  int fd() const { return fd_; }

  const char *node() const { return node_name; }

  const char *mac() const { return mac_; }

  void attach(App *owner) { app = owner; }

  int feed(const uint8_t *buf, size_t len) {
    if (fd_ < 0) return -1;
    handleReport(buf, len);
    return 0;
  }

  // Pure parser for tests and fuzzing: no fd or App required.
  int parse(const uint8_t *buf, size_t len) {
    handleReport(buf, len);
    return 0;
  }
};
