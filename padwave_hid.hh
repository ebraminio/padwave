// padwave_hid.hh — one sysfs scan for the controller node and its MAC.
//
// The player, input engine and App all need the same hidraw discovery. A
// single pass over /sys/class/hidraw reads HID_ID and HID_UNIQ once, returning
// both the /dev node and the Bluetooth address. Enumeration is by
// class entry rather than a fixed hidraw0..15 range, and HID_ID is parsed as
// numbers; this follows hidapi's Linux discovery instead of assuming a layout.
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct DualSenseDevice {
  int index = -1;
  char node[64] = "";
  char mac[32] = "";

  DualSenseDevice() { find(); }

  explicit operator bool() const { return index >= 0; }

  // Enumerate the hidraw class rather than assuming hidraw0..hidrawN are
  // contiguous. Match only by HID_ID's vendor/product, as hidapi does; this
  // avoids HID_NAME or sysfs ordering deciding which device is the pad.
  bool find() {
    index = -1;
    node[0] = 0;
    mac[0] = 0;

    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return false;

    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir))) {
      if (strncmp(entry->d_name, "hidraw", 6) != 0) continue;

      char uevent[160];
      snprintf(uevent, sizeof(uevent), "/sys/class/hidraw/%s/device/uevent", entry->d_name);
      FILE *f = fopen(uevent, "r");
      if (!f) continue;

      bool id_seen = false;
      bool id_match = false;
      char uniq[64] = "";
      char line[256];
      while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "HID_ID=", 7) == 0) {
          unsigned vendor = 0, product = 0;
          id_seen = true;
          if (sscanf(line + 7, "%*x:%x:%x", &vendor, &product) == 2 &&
              (vendor & 0xFFFF) == SONY_VID &&
              ((product & 0xFFFF) == DS_PID || (product & 0xFFFF) == DS_EDGE_PID))
            id_match = true;
        } else if (strncmp(line, "HID_UNIQ=", 9) == 0) {
          snprintf(uniq, sizeof(uniq), "%.*s", (int)sizeof(uniq) - 1, line + 9);
          char *nl = strchr(uniq, '\n');
          if (nl) *nl = 0;
        }
      }
      fclose(f);
      if (!id_seen || !id_match) continue;

      index = atoi(entry->d_name + 6);
      snprintf(node, sizeof(node), "/dev/%s", entry->d_name);
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
      found = true;
    }
    closedir(dir);
    return found;
  }

  int open() const { return node[0] ? ::open(node, O_RDWR | O_CLOEXEC) : -1; }

private:
  static constexpr int SONY_VID = 0x054C;
  static constexpr int DS_PID = 0x0CE6;
  static constexpr int DS_EDGE_PID = 0x0DF2;
};
