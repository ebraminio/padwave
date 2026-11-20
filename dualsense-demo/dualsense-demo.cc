// A standalone DualSense client built on dualsense.hh and hidapi.
//
// Build with the xmake project in dualsense-demo/:
//
//   cd dualsense-demo
//   xmake f -c -y
//   xmake -y
//   xmake run dualsense-demo
//
// Opus and HIDAPI are pinned Git submodules, compiled directly by xmake.
//
// Pipe raw s16le 48 kHz stereo PCM on stdin to stream it to the controller's
// Bluetooth speaker:
//
// ffmpeg -i track.mp3 -f s16le -ac 2 -ar 48000 - | xmake run dualsense-demo
//
// Without a pipe it just shows controller input. On a terminal:
//   l = cycle lightbar, r = rumble, p = cycle player LEDs,
//   t = left trigger feedback, h = help, q = quit.

#ifndef DS_HAVE_OPUS
#define DS_HAVE_OPUS
#endif

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#endif

#include <hidapi.h>

#include "dualsense.hh"

namespace {

constexpr uint16_t kVendorSony           = 0x054c;
constexpr uint16_t kProductDualSense     = 0x0ce6;
constexpr uint16_t kProductDualSenseEdge = 0x0df2;
constexpr int kPcmFrames                 = 480;
constexpr size_t kPcmBytes               = static_cast<size_t>(kPcmFrames) * 2 * sizeof(int16_t);
constexpr uint64_t kReportIntervalUs     = 10667;

#if defined(_WIN32)
constexpr int kStdin  = 0;
constexpr int kStdout = 1;
#define demo_isatty _isatty
#else
constexpr int kStdin  = STDIN_FILENO;
constexpr int kStdout = STDOUT_FILENO;
#define demo_isatty isatty
#endif

volatile sig_atomic_t g_running = 1;
#if !defined(_WIN32)
termios g_saved_tty{};
bool g_tty_changed = false;
#endif

struct Device {
  hid_device *handle = nullptr;
  bool usb           = false;
  char path[512]     = "";
  dualsense::OutputReport::Sequence sequence;
};

struct Options {
  bool help         = false;
  bool list         = false;
  bool input_only   = false;
  int route         = 3; // AudioOutputPath::SpeakerMono
  bool has_lightbar = false;
  int red = 0, green = 0, blue = 0;
  bool has_leds        = false;
  unsigned led_mask    = 0;
  bool has_trigger     = false;
  bool trigger_left    = true;
  int trigger_position = 2;
  int trigger_strength = 6;
  bool has_rumble      = false;
  int rumble_left = 0, rumble_right = 0, rumble_ms = 300;
};

Device g_device;
uint64_t g_rumble_until_ms = 0;
char g_last_status[400]    = "";

void stop(int) { g_running = 0; }

uint64_t nowUs() {
#if defined(_WIN32)
  LARGE_INTEGER frequency{};
  LARGE_INTEGER counter{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return static_cast<uint64_t>(counter.QuadPart) * 1000000ULL /
         static_cast<uint64_t>(frequency.QuadPart);
#else
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
#endif
}

uint64_t nowMs() { return nowUs() / 1000ULL; }

int readPcm(uint8_t *dst, size_t capacity) {
#if defined(_WIN32)
  HANDLE input    = GetStdHandle(STD_INPUT_HANDLE);
  DWORD available = 0;
  if (PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
    if (available == 0) return -2;
    if (available < capacity) capacity = available;
    DWORD got = 0;
    if (!ReadFile(input, dst, static_cast<DWORD>(capacity), &got, nullptr)) return -1;
    return static_cast<int>(got);
  }
  return _read(kStdin, dst, static_cast<unsigned>(capacity));
#else
  return static_cast<int>(read(kStdin, dst, capacity));
#endif
}

void restoreTty() {
#if !defined(_WIN32)
  if (g_tty_changed) {
    tcsetattr(kStdin, TCSANOW, &g_saved_tty);
    g_tty_changed = false;
  }
#endif
}

void setupTty() {
#if !defined(_WIN32)
  if (!demo_isatty(kStdin)) return;
  if (tcgetattr(kStdin, &g_saved_tty) < 0) return;
  termios raw = g_saved_tty;
  raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  raw.c_cc[VMIN]  = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(kStdin, TCSANOW, &raw) == 0) g_tty_changed = true;
#endif
}

void printHelp(const char *argv0) {
  printf("usage: %s [options]\n", argv0);
  printf("  raw s16le 48 kHz stereo on stdin -> Bluetooth 0x36 speaker\n");
  printf("  --route N            audio output path 0..3 (default 3 = speaker)\n");
  printf("  --lightbar R,G,B     set the lightbar\n");
  printf("  --leds MASK          player LED mask, bit 5 = instant\n");
  printf("  --trigger S,P,G      S=left|right, P=position, G=strength\n");
  printf("  --rumble L,R[,MS]    motors for MS milliseconds (default 300)\n");
  printf("  --list               list Sony HID interfaces and exit\n");
  printf("  --input-only         never read PCM from stdin\n");
  printf("  --help               this text\n");
#if !defined(_WIN32)
  printf("\nterminal keys: l lightbar, r rumble, p player LEDs, t trigger, q quit\n");
#endif
}

bool parseArgs(int argc, char **argv, Options &options) {
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--help") == 0) {
      options.help = true;
    } else if (strcmp(arg, "--list") == 0) {
      options.list = true;
    } else if (strcmp(arg, "--input-only") == 0) {
      options.input_only = true;
    } else if (strcmp(arg, "--route") == 0 && i + 1 < argc) {
      options.route = atoi(argv[++i]);
      if (options.route < 0 || options.route > 3) return false;
    } else if (strcmp(arg, "--lightbar") == 0 && i + 1 < argc) {
      if (sscanf(argv[++i], "%d,%d,%d", &options.red, &options.green, &options.blue) != 3)
        return false;
      options.has_lightbar = true;
    } else if (strcmp(arg, "--leds") == 0 && i + 1 < argc) {
      options.led_mask = static_cast<unsigned>(strtoul(argv[++i], nullptr, 0));
      options.has_leds = true;
    } else if (strcmp(arg, "--trigger") == 0 && i + 1 < argc) {
      char side[16] = "";
      if (sscanf(argv[++i], "%15[^,],%d,%d", side, &options.trigger_position,
                 &options.trigger_strength) != 3)
        return false;
      options.trigger_left = strcmp(side, "right") != 0;
      options.has_trigger  = true;
    } else if (strcmp(arg, "--rumble") == 0 && i + 1 < argc) {
      int count = sscanf(argv[++i], "%d,%d,%d", &options.rumble_left, &options.rumble_right,
                         &options.rumble_ms);
      if (count < 2) return false;
      if (count == 2) options.rumble_ms = 300;
      options.has_rumble = true;
    } else {
      return false;
    }
  }
  return true;
}

void listDevices() {
  hid_device_info *list = hid_enumerate(kVendorSony, 0);
  for (const hid_device_info *it = list; it; it = it->next) {
    printf("%04x:%04x bus=%d if=%d usage=%04x:%04x path=%s\n", it->vendor_id, it->product_id,
           static_cast<int>(it->bus_type), it->interface_number, it->usage_page, it->usage,
           it->path ? it->path : "");
  }
  hid_free_enumeration(list);
}

bool openDevice() {
  hid_device_info *list = hid_enumerate(kVendorSony, 0);
  if (!list) {
    fprintf(stderr, "dualsense-demo: no Sony HID device found\n");
    return false;
  }
  const hid_device_info *chosen = nullptr;
  for (const hid_device_info *it = list; it; it = it->next) {
    if (it->product_id != kProductDualSense && it->product_id != kProductDualSenseEdge) continue;
    if (!chosen) chosen = it;
    if (it->usage_page == 1 && it->usage == 5) {
      chosen = it;
      if (it->bus_type == HID_API_BUS_BLUETOOTH) break;
    }
  }
  if (!chosen) {
    hid_free_enumeration(list);
    fprintf(stderr, "dualsense-demo: no DualSense found\n");
    return false;
  }
  snprintf(g_device.path, sizeof(g_device.path), "%s", chosen->path ? chosen->path : "");
  g_device.usb    = chosen->bus_type == HID_API_BUS_USB;
  g_device.handle = hid_open_path(g_device.path);
  if (!g_device.handle) {
    fprintf(stderr, "dualsense-demo: hid_open_path(%s): %ls\n", g_device.path, hid_error(nullptr));
    hid_free_enumeration(list);
    return false;
  }
  printf("controller: %s (%s)\n", g_device.path, g_device.usb ? "usb" : "bluetooth");
  hid_free_enumeration(list);
  return true;
}

template <typename Fill>
bool sendOutput(Fill fill) {
  if (!g_device.handle) return false;
  if (g_device.usb) {
    dualsense::UsbOutputReport report{};
    fill(report.getBody());
    return hid_write(g_device.handle, reinterpret_cast<const uint8_t *>(&report), sizeof report) >
           0;
  } else {
    dualsense::OutputReport report(g_device.sequence);
    fill(report.getBody());
    report.sign();
    return hid_write(g_device.handle, reinterpret_cast<const uint8_t *>(&report), sizeof report) >
           0;
  }
}

bool sendInitialOutput(const Options &options) {
  return sendOutput([&](dualsense::OutputBody &body) {
    body.setAudioControl(static_cast<uint8_t>(options.route << 4));
    body.setHeadphoneVolume(0x7F);
    body.setSpeakerVolume(0x64);
    body.setSpeakerPreampControl(dualsense::OutputBody::SpeakerPreamp::Gain6dB);
    if (options.has_lightbar)
      body.setLightbar(static_cast<uint8_t>(options.red), static_cast<uint8_t>(options.green),
                       static_cast<uint8_t>(options.blue));
    if (options.has_leds) {
      body.setPlayerLeds((options.led_mask & 0x01) != 0, (options.led_mask & 0x02) != 0,
                         (options.led_mask & 0x04) != 0, (options.led_mask & 0x08) != 0,
                         (options.led_mask & 0x10) != 0, (options.led_mask & 0x20) != 0);
    }
    if (options.has_trigger) {
      auto effect = dualsense::OutputBody::TriggerEffect::feedback(
          static_cast<uint8_t>(options.trigger_position),
          static_cast<uint8_t>(options.trigger_strength));
      if (options.trigger_left)
        body.setLeftTriggerEffect(effect);
      else
        body.setRightTriggerEffect(effect);
    }
    if (options.has_rumble)
      body.setCompatibleVibration(static_cast<uint8_t>(options.rumble_left),
                                  static_cast<uint8_t>(options.rumble_right));
  });
}

bool sendRumble(int left, int right, int milliseconds) {
  if (!sendOutput([&](dualsense::OutputBody &body) {
        body.setCompatibleVibration(static_cast<uint8_t>(left), static_cast<uint8_t>(right));
      }))
    return false;
  g_rumble_until_ms = milliseconds > 0 ? nowMs() + static_cast<uint64_t>(milliseconds) : 0;
  return true;
}

const char *dpadName(dualsense::InputBody::Dpad dpad) {
  using Dpad = dualsense::InputBody::Dpad;
  switch (dpad) {
  case Dpad::Up: return "up";
  case Dpad::UpRight: return "up-right";
  case Dpad::Right: return "right";
  case Dpad::DownRight: return "down-right";
  case Dpad::Down: return "down";
  case Dpad::DownLeft: return "down-left";
  case Dpad::Left: return "left";
  case Dpad::UpLeft: return "up-left";
  case Dpad::None: return "none";
  }
  return "?";
}

void showInput(const dualsense::InputBody &input) {
  using Button = dualsense::InputBody::Button;
  static const struct {
    Button button;
    const char *name;
  } names[] = {
      {Button::ButtonSquare, "square"},
      {Button::ButtonCross, "cross"},
      {Button::ButtonCircle, "circle"},
      {Button::ButtonTriangle, "triangle"},
      {Button::L1, "l1"},
      {Button::R1, "r1"},
      {Button::L2, "l2"},
      {Button::R2, "r2"},
      {Button::Create, "create"},
      {Button::Options, "options"},
      {Button::L3, "l3"},
      {Button::R3, "r3"},
      {Button::PsHome, "ps"},
      {Button::Touchpad, "touchpad"},
      {Button::MicMute, "mute"},
      {Button::EdgeFn1, "fn1"},
      {Button::EdgeFn2, "fn2"},
      {Button::EdgeLeftPaddle, "paddle-l"},
      {Button::EdgeRightPaddle, "paddle-r"},
  };
  char buttons[192]  = "";
  size_t used        = 0;
  const auto pressed = input.getButtons();
  for (const auto &entry : names) {
    if (!pressed.isOn(entry.button)) continue;
    int written =
        snprintf(buttons + used, sizeof(buttons) - used, "%s%s", used ? "," : "", entry.name);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(buttons) - used) break;
    used += static_cast<size_t>(written);
  }
  const auto left     = input.getLeftStick();
  const auto right    = input.getRightStick();
  const auto triggers = input.getTriggers();
  char line[400];
  snprintf(line, sizeof(line), "[in] %s dpad=%s l2=%d r2=%d L=(%d,%d) R=(%d,%d) batt=%d%%",
           buttons[0] ? buttons : "-", dpadName(input.getDpad()), triggers.left, triggers.right,
           left.x, left.y, right.x, right.y, input.getBattery());
  if (strcmp(line, g_last_status) == 0) return;
  snprintf(g_last_status, sizeof(g_last_status), "%s", line);
  if (demo_isatty(kStdout))
    printf("\r\033[K%s", line);
  else
    printf("%s\n", line);
  fflush(stdout);
}

#if !defined(_WIN32)
void handleKey(char key) {
  static int color                 = 0;
  static int led_mask              = 0;
  static bool trigger_on           = false;
  static const uint8_t colors[][3] = {
      {0x00, 0x40, 0xFF}, {0x00, 0xFF, 0x40}, {0xFF, 0x40, 0x00}, {0xFF, 0x00, 0x40}};
  if (key == 'q') {
    g_running = 0;
    return;
  }
  if (key == 'h') {
    printf("\nterminal keys: l lightbar, r rumble, p player LEDs, t trigger, q quit\n");
    g_last_status[0] = 0;
    return;
  }
  if (key == 'l') {
    color = (color + 1) % 4;
    sendOutput([&](dualsense::OutputBody &body) {
      body.setLightbar(colors[color][0], colors[color][1], colors[color][2]);
    });
  } else if (key == 'r') {
    sendRumble(120, 120, 300);
  } else if (key == 'p') {
    led_mask = (led_mask + 1) % 6;
    sendOutput([&](dualsense::OutputBody &body) {
      const unsigned mask = 1U << led_mask;
      body.setPlayerLeds((mask & 0x01) != 0, (mask & 0x02) != 0, (mask & 0x04) != 0,
                         (mask & 0x08) != 0, (mask & 0x10) != 0, false);
    });
  } else if (key == 't') {
    trigger_on  = !trigger_on;
    auto effect = trigger_on ? dualsense::OutputBody::TriggerEffect::feedback(2, 6)
                             : dualsense::OutputBody::TriggerEffect::off();
    sendOutput([&](dualsense::OutputBody &body) { body.setLeftTriggerEffect(effect); });
  }
}
#endif

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parseArgs(argc, argv, options)) {
    printHelp(argv[0]);
    return 2;
  }
  if (options.help) {
    printHelp(argv[0]);
    return 0;
  }
  if (hid_init() != 0) {
    fprintf(stderr, "dualsense-demo: hid_init failed\n");
    return 1;
  }
  if (options.list) {
    listDevices();
    hid_exit();
    return 0;
  }

  signal(SIGINT, stop);
  signal(SIGTERM, stop);
  if (!openDevice()) {
    hid_exit();
    return 1;
  }
  if (!sendInitialOutput(options)) {
    fprintf(stderr, "dualsense-demo: initial output report failed: %ls\n",
            hid_error(g_device.handle));
    hid_close(g_device.handle);
    hid_exit();
    return 1;
  }

  const bool piped = !options.input_only && !demo_isatty(kStdin);
  if (piped && g_device.usb) {
    fprintf(stderr, "dualsense-demo: 0x36 audio streaming is Bluetooth-only\n");
    hid_close(g_device.handle);
    hid_exit();
    return 1;
  }
  if (options.has_rumble) g_rumble_until_ms = nowMs() + static_cast<uint64_t>(options.rumble_ms);

  if (!piped) setupTty();
  if (piped) {
#if defined(_WIN32)
    _setmode(kStdin, _O_BINARY);
#else
    int flags = fcntl(kStdin, F_GETFL, 0);
    if (flags >= 0) fcntl(kStdin, F_SETFL, flags | O_NONBLOCK);
#endif
    printf("streaming s16le 48 kHz stereo from stdin; Ctrl-C to stop\n");
  } else {
#if defined(_WIN32)
    if (g_device.usb)
      printf("Windows USB: input/output reports only; 0x36 audio is Bluetooth-only\n");
    else
      printf("Windows build: pipe PCM for Bluetooth 0x36 audio\n");
#else
    printf("waiting for controller input; h for keys, Ctrl-C to quit\n");
#endif
  }

  dualsense::AudioReport audio;
  dualsense::AudioReport::Encoder encoder;
  bool have_audio = false;
  if (piped) {
    if (!encoder.open(48000, 2)) {
      fprintf(stderr, "dualsense-demo: Opus encoder init failed\n");
      restoreTty();
      hid_close(g_device.handle);
      hid_exit();
      return 1;
    }
    audio.setSpeakTag(options.route >= 2 ? dualsense::AudioReport::SpeakerTag::Speaker
                                         : dualsense::AudioReport::SpeakerTag::Jack);
    audio.setConfig(dualsense::AudioReport::ConfigMask::SpeakerOnly);
    audio.setHeadphoneVolume(0x7F);
    audio.setSpeakerVolume(0x64);
    have_audio = true;
  }

  uint8_t pcm_bytes[kPcmBytes];
  size_t pcm_have         = 0;
  bool audio_started      = false;
  bool audio_eof          = false;
  uint64_t next_audio_us  = 0;
  unsigned long underruns = 0;
  dualsense::OutputReport::Sequence audio_sequence;

  while (g_running) {
    uint8_t report[512];
    int bytesRead = hid_read_timeout(g_device.handle, report, sizeof report, 1);
    if (bytesRead > 0) {
      if (const auto *input =
              dualsense::InputReport::asFrame(report, static_cast<size_t>(bytesRead))) {
        showInput(input->getBody());
      } else if (const auto *input =
                     dualsense::UsbInputReport::asFrame(report, static_cast<size_t>(bytesRead))) {
        showInput(input->getBody());
      }
    }

#if !defined(_WIN32)
    if (!piped) {
      char key = 0;
      if (read(kStdin, &key, 1) == 1) handleKey(key);
    }
#endif
    if (piped) {
      if (!audio_eof && pcm_have < sizeof(pcm_bytes)) {
        int bytes = readPcm(pcm_bytes + pcm_have, sizeof(pcm_bytes) - pcm_have);
        if (bytes > 0)
          pcm_have += static_cast<size_t>(bytes);
        else if (bytes == 0)
          audio_eof = true;
#if !defined(_WIN32)
        else if (errno != EAGAIN && errno != EINTR)
          audio_eof = true;
#else
        else if (bytes != -2)
          audio_eof = true;
#endif
      }
      uint64_t now = nowUs();
      if (audio_eof && audio_started && pcm_have == 0) have_audio = false;
      if (!audio_started && pcm_have >= sizeof(pcm_bytes)) {
        audio_started = true;
        next_audio_us = now;
      }
      if (have_audio && audio_started && now >= next_audio_us) {
        const bool tail = audio_eof && pcm_have < sizeof(pcm_bytes);
        float pcm[kPcmFrames * 2];
        for (int i = 0; i < kPcmFrames * 2; ++i) {
          int16_t sample = 0;
          if (static_cast<size_t>(i) * sizeof(int16_t) + sizeof(int16_t) <= pcm_have)
            memcpy(&sample, pcm_bytes + static_cast<size_t>(i) * sizeof(int16_t), sizeof(sample));
          pcm[i] = static_cast<float>(sample) / 32768.0f;
        }
        if (pcm_have < sizeof(pcm_bytes) && !tail) ++underruns;
        pcm_have = 0;

        audio.setSequence(audio_sequence);
        audio.setHaptics([](size_t) { return int8_t{0}; });
        audio.setPcm(pcm, kPcmFrames, encoder);
        audio.sign();
        if (hid_write(g_device.handle, reinterpret_cast<const unsigned char *>(&audio),
                      sizeof(audio)) <= 0) {
          fprintf(stderr, "dualsense-demo: audio write failed: %ls\n", hid_error(g_device.handle));
          break;
        }
        if (tail) {
          have_audio = false;
          printf("\nPCM EOF (%lu underruns)\n", underruns);
        }
        next_audio_us += kReportIntervalUs;
        if (next_audio_us < now) next_audio_us = now + kReportIntervalUs;
      }
    }

    if (g_rumble_until_ms && nowMs() >= g_rumble_until_ms) {
      g_rumble_until_ms = 0;
      sendRumble(0, 0, 0);
    }
  }

  if (demo_isatty(kStdout)) printf("\n");
  restoreTty();
  hid_close(g_device.handle);
  hid_exit();
  return 0;
}
