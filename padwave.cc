// padwave — one translation unit: the app, its player and input engines, and
// the web server. The wire formats live in padwave_wire.hh.
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "padwave_app.hh"
#include "padwave_server.hh"

// Shared helper: finding the controller's hidraw node.

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

int server_main(int argc, char **argv) {
  static Server server{}; // connection table in BSS; the App stays on the stack
  int port = argc > 1 ? atoi(argv[1]) : 8787;
  return server.run(port);
}

// padwave — one binary, one process, one thread.
//
//   padwave [port]    the app: panel, radio, player and input decoder
//
// All of it is in this translation unit; padwave_server.hh, padwave_app.hh,
// padwave_player.hh and padwave_input.hh describe the parts. The wire format
// is in padwave_wire.hh, and every part uses the same schema.
int main(int argc, char **argv) { return server_main(argc, argv); }
