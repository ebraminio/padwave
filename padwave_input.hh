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

#include "dualsense.hh"

// padwave input — the controller's state, decoded by the C++ schema.
// The engine is in-process; the app owns one Input and attaches itself.

struct App;

struct Input {
  // Press-and-hold threshold for PS → suspend/power off.
  static constexpr int PsHoldMs = 2000;

private:
  struct ActionsState {
    dualsense::InputBody::Buttons prevButtons = dualsense::InputBody::Buttons();
    int64_t last_l2 = 0;
    int64_t last_r2 = 0;
    int64_t ps_at = 0;
    bool ps_armed = false;
  };

  struct SensorsState {
    bool has_prev = false;
    dualsense::InputBody::Buttons prevButtons = dualsense::InputBody::Buttons();
    dualsense::InputBody::Dpad prevDpad = dualsense::InputBody::Dpad::None;
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
    dualsense::InputBody::Dpad prev = dualsense::InputBody::Dpad::None;
    int hold_dir = 0;
    int hold_step = 0;
    int64_t hold_at = 0;
  };

  struct Binding {
    dualsense::InputBody::Button button;
    const char *action;
  };

  int fd_ = -1;
  char node_name[64] = "";
  char mac_[32] = "";
  App *app = nullptr;
#ifdef DS_HAVE_OPUS
  dualsense::MicFrame::Decoder mic_decoder;
#endif

  ActionsState actions;
  SensorsState sensors;
  DpadState dpad_state;

  static constexpr Binding kBindings[] = {
      {dualsense::InputBody::Button::ButtonTriangle, "toggle_pause"},
      {dualsense::InputBody::Button::ButtonCircle, "play"},
      {dualsense::InputBody::Button::ButtonSquare, "shuffle_folder"},
      {dualsense::InputBody::Button::L1, "previous"},
      {dualsense::InputBody::Button::R1, "next"},
      {dualsense::InputBody::Button::Create, "previous"},
      {dualsense::InputBody::Button::Options, "next"},
      {dualsense::InputBody::Button::Touchpad, "toggle_mute"},
      {dualsense::InputBody::Button::MicMute, "toggle_mute"},
      {dualsense::InputBody::Button::L3, "next"},
      {dualsense::InputBody::Button::R3, "previous"},
      {dualsense::InputBody::Button::EdgeLeftPaddle, "previous"},
      {dualsense::InputBody::Button::EdgeRightPaddle, "next"},
      {dualsense::InputBody::Button::EdgeFn1, "toggle_pause"},
      {dualsense::InputBody::Button::EdgeFn2, "toggle_mute"},
  };

  void trackButtons(dualsense::InputBody::Buttons buttons) {
    dualsense::InputBody::Buttons pressed = buttons.pressed(actions.prevButtons);
    actions.prevButtons = buttons;

    for (auto &b : kBindings)
      if (pressed.isOn(b.button)) emitAction(b.action, 0);

    if (pressed.isOn(dualsense::InputBody::Button::PsHome)) {
      actions.ps_armed = true;
      actions.ps_at = nowMs();
    }

    if (!buttons.isOn(dualsense::InputBody::Button::PsHome)) {
      actions.ps_armed = false;
    } else if (actions.ps_armed && nowMs() - actions.ps_at >= PsHoldMs) {
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

  void trackSensors(const dualsense::InputBody &b) {
    int64_t now = nowMs();
    bool jack = b.headset();
    const dualsense::InputBody::Stick left = b.leftStick();
    const dualsense::InputBody::Stick right = b.rightStick();
    const dualsense::InputBody::Triggers triggers = b.triggers();
    int l2 = triggers.left, r2 = triggers.right;
    int lx = left.x, ly = left.y, rx = right.x, ry = right.y;
    const dualsense::InputBody::Buttons buttons = b.buttons();
    dualsense::InputBody::Dpad dpad = b.dpad();
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

  void trackDpad(dualsense::InputBody::Dpad dpad) {
    static const int steps[] = {10, 20, 40, 60};
    static const int intervals[] = {320, 260, 190, 130};

    int64_t now = nowMs();

    if (dpad != dpad_state.prev) {
      dpad_state.prev = dpad;
      dpad_state.hold_dir = 0;

      if (dpad == dualsense::InputBody::Dpad::Left) {
        emitAction("seek", -6);
        dpad_state.hold_dir = -1;
        dpad_state.hold_step = 0;
        dpad_state.hold_at = now + 420;
      } else if (dpad == dualsense::InputBody::Dpad::Right) {
        emitAction("seek", 6);
        dpad_state.hold_dir = 1;
        dpad_state.hold_step = 0;
        dpad_state.hold_at = now + 420;
      } else if (dpad == dualsense::InputBody::Dpad::Up) {
        emitAction("next", 0);
      } else if (dpad == dualsense::InputBody::Dpad::Down) {
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
  void emitMic(const int16_t *pcm, size_t samples);
  void emitFrameToApp(const dualsense::InputBody &b, const uint8_t *raw, size_t len);

  void emitFrame(const dualsense::InputBody &b, const uint8_t *raw, size_t len) {
    emitFrameToApp(b, raw, len);
    trackDpad(b.dpad());
    trackButtons(b.buttons());
    const dualsense::InputBody::Triggers triggers = b.triggers();
    trackTriggers(triggers.left, triggers.right);
    trackSensors(b);
  }

  void handleReport(const uint8_t *buf, size_t len) {
    if (!buf || len == 0) return;

#ifdef DS_HAVE_OPUS
    if (auto *audioFrame = dualsense::MicFrame::asFrame(buf, len)) {
      size_t payload_len = 0;
      const uint8_t *payload = audioFrame->payload(buf, len, &payload_len);
      if (!mic_decoder.valid() && !mic_decoder.open(dualsense::MicFrame::sampleRate, 1)) return;
      int16_t pcm[dualsense::MicFrame::samples];
      int n = mic_decoder.decode(payload, payload_len, pcm, dualsense::MicFrame::samples);
      if (n > 0) emitMic(pcm, static_cast<size_t>(n));
      return;
    }
#endif

    if (auto *r = dualsense::InputReport::asFrame(buf, len)) {
      emitFrame(r->body(), buf, sizeof(*r));
      return;
    }

    if (auto *r = dualsense::UsbInputReport::asFrame(buf, len)) {
      emitFrame(r->body(), buf, sizeof(*r));
    }
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
#ifdef DS_HAVE_OPUS
    mic_decoder.close();
#endif
    actions = ActionsState{};
    sensors = SensorsState{};
    dpad_state = DpadState{};
  }

  int fd() const { return fd_; }

  bool micReady() const {
#ifdef DS_HAVE_OPUS
    return mic_decoder.valid();
#else
    return false;
#endif
  }

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
