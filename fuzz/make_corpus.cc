// Build the seed corpus into the directory given as argv[1]: a signed
// Bluetooth 0x31 report, a USB 0x01 report and a mic frame.
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

int main(int argc, char **argv) {
  if (argc < 2) return 1;

  padwave::InputReport bt{};
  bt.report_id = padwave::InputReport::ReportId;
  bt.seq_tag = 0x10;
  bt.body.lx = 128;
  bt.body.ly = 128;
  bt.body.rx = 128;
  bt.body.ry = 128;
  bt.body.status0 = 0x15;  // battery 50%, discharging
  bt.body.temperature = 30;
  bt.sign();

  uint8_t usb[padwave::InputReport::UsbSize] = {padwave::InputReport::UsbReportId};
  usb[1 + 0] = 0x80;
  usb[1 + 1] = 0x80;

  uint8_t mic[78] = {0};
  mic[0] = padwave::MicFrame::ReportId;
  mic[3] = padwave::MicFrame::Toc;

  bool ok = write_file(argv[1], "bt31.bin", &bt, sizeof(bt)) &&
            write_file(argv[1], "usb01.bin", usb, sizeof(usb)) &&
            write_file(argv[1], "mic.bin", mic, sizeof(mic));
  return ok ? 0 : 1;
}
