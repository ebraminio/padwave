// Build the seed corpus into the directory given as argv[1]: signed 0x31
// reports (neutral, button pressed, button released), a USB 0x01 report and a
// mic frame.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "padwave_wire.hh"

static bool write_file(const char *dir, const char *name, const void *data, size_t len) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == len;
  fclose(f);
  return ok;
}

static padwave::InputReport report(uint32_t buttons) {
  padwave::InputReport r{};
  auto *raw = reinterpret_cast<uint8_t *>(&r);
  r.report_id = padwave::InputReport::ReportId;
  r.seq_tag = 0x10;
  r.body.lx = 128;
  r.body.ly = 128;
  r.body.rx = 128;
  r.body.ry = 128;
  r.body.status0 = 0x15;
  r.body.temperature = 30;
  r.body.buttons = buttons;
  r.sign();
  return r;
}

int main(int argc, char **argv) {
  if (argc < 2) return 1;

  padwave::InputReport neutral = report(0);
  padwave::InputReport pressed = report(padwave::InputBody::Button::ButtonCross);
  padwave::InputReport released = report(0);

  uint8_t usb[padwave::InputReport::UsbSize] = {padwave::InputReport::UsbReportId};
  usb[1 + 0] = 0x80;
  usb[1 + 1] = 0x80;

  uint8_t mic[78] = {0};
  mic[0] = padwave::MicFrame::ReportId;
  mic[3] = padwave::MicFrame::Toc;

  bool ok = write_file(argv[1], "bt31.bin", &neutral, sizeof(neutral)) &&
            write_file(argv[1], "press.bin", &pressed, sizeof(pressed)) &&
            write_file(argv[1], "release.bin", &released, sizeof(released)) &&
            write_file(argv[1], "usb01.bin", usb, sizeof(usb)) &&
            write_file(argv[1], "mic.bin", mic, sizeof(mic));
  return ok ? 0 : 1;
}
