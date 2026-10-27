#pragma once
// The tiny status bus from the native processes to the web front.
//
// Unix datagrams, one line each, fixed field order so the receiver can use
// sscanf. Sending is fire-and-forget: when the front is not running the
// sendto() is dropped and nothing else changes.
//
//   player <pos_ms> <dur_ms> <sent> <underruns>
//   input  <lx> <ly> <rx> <ry> <l2> <r2> <buttons> <battery> <jack>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace padwave_status {

static constexpr const char *kPath = "/tmp/padwave-status.sock";

static inline int sender_open() {
  return socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

static inline void send_line(int fd, const char *line) {
  if (fd < 0) return;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", kPath);
  (void)sendto(fd, line, strlen(line), 0, (struct sockaddr *)&addr, sizeof(addr));
}

} // namespace padwave_status
