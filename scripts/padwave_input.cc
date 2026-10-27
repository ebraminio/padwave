// padwave input — the controller's state, decoded by the C++ schema.
//
// A separate process from the player on purpose: reading input is independent
// of whether audio is playing, and the player should not own the input fd just
// because it happens to be the process that also writes 0x36. This CLI is the
// seam for moving the input path out of Elixir: it parses 0x31 (Bluetooth) and
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "padwave_schemas.hh"

namespace {

int find_dualsense(char *node, size_t len) {
  for (int i = 0; i < 8; i++) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/hidraw/hidraw%d/device/uevent", i);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    char line[256];
    bool match = false;
    while (fgets(line, sizeof(line), f))
      if (strstr(line, "054C:00000CE6") || strstr(line, "054C:00000DF2")) match = true;
    fclose(f);
    if (!match) continue;
    snprintf(node, len, "/dev/hidraw%d", i);
    return i;
  }
  return -1;
}

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

void print_frame(const padwave::InputBody &b) {
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
  printf(" t1=%02x%02x%02x%02x t2=%02x%02x%02x%02x\n", (unsigned)b.touch1[0].get(),
         (unsigned)b.touch1[1].get(), (unsigned)b.touch1[2].get(), (unsigned)b.touch1[3].get(),
         (unsigned)b.touch2[0].get(), (unsigned)b.touch2[1].get(), (unsigned)b.touch2[2].get(),
         (unsigned)b.touch2[3].get());
  fflush(stdout);
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
    print_frame(r->body);
    return;
  }

  if (buf[0] == padwave::PacketInputUsb && len >= padwave::InputReport::UsbSize)
    print_frame(*(const padwave::InputBody *)(buf + 1));
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

int input_main(int argc, char **argv) {
  uint8_t buf[512];

  if (argc > 1 && strcmp(argv[1], "--hex") == 0 && argc > 2) {
    size_t len = 0;
    if (!hex_decode(argv[2], buf, &len)) return 2;
    // A btmon line often includes the 0xA2/0xA1 transaction prefix.
    if (len > 1 && (buf[0] == 0xA1 || buf[0] == 0xA2)) {
      memmove(buf, buf + 1, len - 1);
      len--;
    }
    handle_report(buf, len);
    return 0;
  }

  int fd = -1;
  if (argc > 1 && strcmp(argv[1], "--file") == 0 && argc > 2) {
    fd = open(argv[2], O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 2;
    for (;;) {
      ssize_t n = read(fd, buf, 78);
      if (n <= 0) break;
      handle_report(buf, (size_t)n);
    }
    close(fd);
    return 0;
  }

  if (argc > 1)
    fd = open(argv[1], O_RDONLY | O_CLOEXEC);
  else {
    char node[64];
    if (find_dualsense(node, sizeof(node)) >= 0) fd = open(node, O_RDONLY | O_CLOEXEC);
  }
  if (fd < 0) {
    fprintf(stderr, "padwave input: no DualSense hidraw node\n");
    return 1;
  }

  printf("ready\n");
  fflush(stdout);

  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    handle_report(buf, (size_t)n);
  }
  close(fd);
  return 0;
}
