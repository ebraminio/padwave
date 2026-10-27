#pragma once

#include <stdio.h>
#include <string.h>
#include <fcntl.h>

static inline int
find_dualsense(char *node, size_t len) {
  /* /sys/class/hidraw/hidrawN/device/uevent carries HID_ID=0005:0000054C:00000CE6 */
  for (int i = 0; i < 16; i++) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/hidraw/hidraw%d/device/uevent", i);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    char line[256];
    bool match = false;
    #define SONY_VID "054C"
    #define DS_PID "0CE6"
    #define DS_EDGE_PID "0DF2"
    while (fgets(line, sizeof(line), f))
      if (strstr(line, SONY_VID ":0000" DS_PID) || strstr(line, SONY_VID ":0000" DS_EDGE_PID)) match = true;
    #undef SONY_VID
    #undef DS_PID
    #undef DS_EDGE_PID
    fclose(f);
    if (!match) continue;
    snprintf(node, len, "/dev/hidraw%d", i);
    return i;
  }
  return -1;
}

static inline
int find_dualsense() {
  char node[64];
  find_dualsense(node, sizeof(node));
  return open(node, O_RDWR | O_CLOEXEC);
}
