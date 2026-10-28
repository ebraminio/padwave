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
};

// The sysfs uevent carries HID_ID=0005:0000054C:00000CE6 for the DualSense and
// ...0DF2 for the Edge. HID_NAME is a fallback for a node whose ID does not
// match; HID_UNIQ is the Bluetooth address.
static inline bool find_dualsense_device(DualsenseDevice *out) {
  if (!out) return false;
  out->index = -1;
  out->node[0] = 0;
  out->mac[0] = 0;

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

    out->index = i;
    snprintf(out->node, sizeof(out->node), "/dev/hidraw%d", i);

    if (strlen(uniq) == 17) {
      bool valid = true;
      for (const char *p = uniq; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') ||
              *p == ':')) {
          valid = false;
          break;
        }
      }
      if (valid) snprintf(out->mac, sizeof(out->mac), "%s", uniq);
    }
    return true;
  }
  return false;
}

static inline int find_dualsense(char *node, size_t len) {
  DualsenseDevice device;
  if (!find_dualsense_device(&device)) return -1;
  if (node && len) snprintf(node, len, "%s", device.node);
  return device.index;
}

static inline int find_dualsense() {
  DualsenseDevice device;
  if (!find_dualsense_device(&device)) return -1;
  return open(device.node, O_RDWR | O_CLOEXEC);
}
