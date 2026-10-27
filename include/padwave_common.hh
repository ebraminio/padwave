#pragma once
// Shared helpers for the native modes: finding the controller's hidraw node
// and writing a whole buffer to it.

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// The sysfs uevent carries HID_ID=0005:0000054C:00000CE6 for the DualSense and
// ...0DF2 for the Edge. Scan a bounded range; each node's uevent is small.
static const char *const kDualsenseIds[] = {"054C:00000CE6", "054C:00000DF2"};

static inline int find_dualsense(char *node, size_t len) {
  for (int i = 0; i < 16; i++) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/hidraw/hidraw%d/device/uevent", i);
    FILE *f = fopen(path, "r");
    if (!f) continue;

    char line[256];
    bool match = false;
    while (fgets(line, sizeof(line), f)) {
      for (const char *id : kDualsenseIds) {
        if (strstr(line, id)) {
          match = true;
          break;
        }
      }
    }
    fclose(f);

    if (!match) continue;
    snprintf(node, len, "/dev/hidraw%d", i);
    return i;
  }
  return -1;
}

// Open the first DualSense. The player needs read/write; callers that only
// want to read should use find_dualsense(node, len) and open it themselves.
static inline int find_dualsense() {
  char node[64];
  if (find_dualsense(node, sizeof(node)) < 0) return -1;
  return open(node, O_RDWR | O_CLOEXEC);
}

static inline bool write_all(int fd, const void *buf, size_t len) {
  const uint8_t *data = (const uint8_t *)buf;
  size_t put = 0;
  while (put < len) {
    ssize_t n = write(fd, data + put, len - put);
    if (n <= 0) return false;
    put += (size_t)n;
  }
  return true;
}
