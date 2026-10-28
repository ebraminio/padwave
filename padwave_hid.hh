// padwave_hid.hh — one sysfs scan for the controller node and its MAC.
//
// The player, input engine and App all need the same hidraw discovery. A
// single pass over /sys/class/hidraw reads HID_ID/HID_NAME and HID_UNIQ once,
// returning both the /dev node and the Bluetooth address.
#pragma once

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct DualsenseDevice {
  int index = -1;
  char node[64] = "";
  char mac[32] = "";

  DualsenseDevice() { find(); }

  explicit operator bool() const { return index >= 0; }

  // One pass over the hidraw uevents. The HID_ID values identify the
  // DualSense/Edge; HID_NAME is a fallback, and HID_UNIQ is the Bluetooth
  // address.
  bool find() {
    index = -1;
    node[0] = 0;
    mac[0] = 0;

    static const char *const ids[] = {"054C:00000CE6", "054C:00000DF2"};
    for (int i = 0; i < 16; i++) {
      char path[128];
      snprintf(path, sizeof(path), "/sys/class/hidraw/hidraw%d/device/uevent", i);
      FILE *f = fopen(path, "r");
      if (!f) continue;

      bool match = false;
      char uniq[64] = "";
      char line[256];
      while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "HID_ID=")) {
          for (const char *id : ids)
            if (strstr(line, id)) match = true;
        }
        if (strstr(line, "HID_NAME=") && strstr(line, "DualSense")) match = true;
        if (strncmp(line, "HID_UNIQ=", 9) == 0) {
          snprintf(uniq, sizeof(uniq), "%.*s", (int)sizeof(uniq) - 1, line + 9);
          char *nl = strchr(uniq, '\n');
          if (nl) *nl = 0;
        }
      }
      fclose(f);
      if (!match) continue;

      index = i;
      snprintf(node, sizeof(node), "/dev/hidraw%d", i);

      if (strlen(uniq) == 17) {
        bool valid = true;
        for (const char *p = uniq; *p; p++) {
          if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') ||
                *p == ':')) {
            valid = false;
            break;
          }
        }
        if (valid) snprintf(mac, sizeof(mac), "%s", uniq);
      }
      return true;
    }
    return false;
  }

  int open() const { return node[0] ? ::open(node, O_RDWR | O_CLOEXEC) : -1; }
};
