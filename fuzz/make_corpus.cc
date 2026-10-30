// Build the seed corpus into the directory given as argv[1]: signed 0x31
// reports (neutral, button pressed, button released), a USB 0x01 report and a
// mic frame.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dualsense.hh"

static bool write_file(const char *dir, const char *name, const void *data, size_t len) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == len;
  fclose(f);
  return ok;
}

static dualsense::InputReport report(uint32_t buttons) {
  dualsense::InputReport r{};
  auto *raw = reinterpret_cast<uint8_t *>(&r);
  raw[0] = dualsense::InputReport::ReportId;
  raw[1] = 0x10;
  raw[2 + 0x00] = 128;
  raw[2 + 0x01] = 128;
  raw[2 + 0x02] = 128;
  raw[2 + 0x03] = 128;
  raw[2 + 0x07] = static_cast<uint8_t>(buttons);
  raw[2 + 0x08] = static_cast<uint8_t>(buttons >> 8);
  raw[2 + 0x09] = static_cast<uint8_t>(buttons >> 16);
  raw[2 + 0x0a] = static_cast<uint8_t>(buttons >> 24);
  raw[2 + 0x1f] = 30;
  raw[2 + 0x34] = 0x15;
  r.sign();
  return r;
}

int main(int argc, char **argv) {
  if (argc < 2) return 1;

  dualsense::InputReport neutral = report(0);
  dualsense::InputReport pressed = report(static_cast<uint32_t>(dualsense::InputBody::Button::ButtonCross));
  dualsense::InputReport released = report(0);

  uint8_t usb[dualsense::InputReport::UsbSize] = {dualsense::InputReport::UsbReportId};
  usb[1 + 0] = 0x80;
  usb[1 + 1] = 0x80;

  uint8_t mic[78] = {0};
  mic[0] = dualsense::MicFrame::ReportId;
  mic[3] = dualsense::MicFrame::Toc;

  bool ok = write_file(argv[1], "bt31.bin", &neutral, sizeof(neutral)) &&
            write_file(argv[1], "press.bin", &pressed, sizeof(pressed)) &&
            write_file(argv[1], "release.bin", &released, sizeof(released)) &&
            write_file(argv[1], "usb01.bin", usb, sizeof(usb)) &&
            write_file(argv[1], "mic.bin", mic, sizeof(mic));
  return ok ? 0 : 1;
}
