// padwave serve — the bootstrap of the C++ web server.
//
// A single epoll loop, level-triggered, with one non-blocking socket per
// connection and a fixed-size request buffer. It serves the panel from the
// HTML embedded in the binary and a placeholder /api/status; the Elixir server
// still owns the real API while this grows.
//
// The UI comes from #embed when the compiler has it (Clang), and from the
// header install.sh generates (GCC 12 has no #embed) otherwise. Either way the
// binary carries the panel and needs no files at run time.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__has_embed)
#if __has_embed("../priv/ui/index.html")
static const unsigned char kIndexHtml[] = {
#embed "../priv/ui/index.html"
};
static const size_t kIndexHtmlSize = sizeof(kIndexHtml);
#else
#include "padwave_ui.hh"
#endif
#else
#include "padwave_ui.hh"
#endif

namespace {

constexpr int kMaxConns = 64;
constexpr size_t kRequestMax = 4096;

struct response {
  char *data;
  size_t len;
};

struct conn {
  int fd = -1;
  char request[kRequestMax];
  size_t request_len = 0;
  const char *out = nullptr;
  size_t out_len = 0;
  size_t out_off = 0;
};

response g_index;
response g_status;
response g_not_found;

response make_response(const char *type, const char *body, size_t body_len) {
  const char *fmt =
      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
      "Connection: close\r\nCache-Control: no-cache\r\n\r\n";
  int head_len = snprintf(nullptr, 0, fmt, type, body_len);
  char *data = (char *)malloc((size_t)head_len + body_len);
  if (!data) {
    response empty{nullptr, 0};
    return empty;
  }
  snprintf(data, (size_t)head_len + 1, fmt, type, body_len);
  memcpy(data + head_len, body, body_len);
  return {data, (size_t)head_len + body_len};
}

response make_error(int code, const char *reason, const char *body) {
  const char *fmt =
      "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
      "Connection: close\r\n\r\n";
  size_t body_len = strlen(body);
  int head_len = snprintf(nullptr, 0, fmt, code, reason, body_len);
  char *data = (char *)malloc((size_t)head_len + body_len);
  if (!data) return {nullptr, 0};
  snprintf(data, (size_t)head_len + 1, fmt, code, reason, body_len);
  memcpy(data + head_len, body, body_len);
  return {data, (size_t)head_len + body_len};
}

const response *route(const char *path) {
  if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) return &g_index;
  if (strcmp(path, "/api/status") == 0) return &g_status;
  return &g_not_found;
}

void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool epoll_add(int epoll_fd, int fd, uint32_t events, void *ptr) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.ptr = ptr;
  return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool epoll_mod(int epoll_fd, int fd, uint32_t events, void *ptr) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.ptr = ptr;
  return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void close_conn(int epoll_fd, conn *c) {
  if (c->fd < 0) return;
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, nullptr);
  close(c->fd);
  c->fd = -1;
  c->request_len = 0;
  c->out = nullptr;
  c->out_len = 0;
  c->out_off = 0;
}

void send_response(int epoll_fd, conn *c, const response *r) {
  c->out = r->data;
  c->out_len = r->len;
  c->out_off = 0;
  epoll_mod(epoll_fd, c->fd, EPOLLOUT, c);
}

void read_request(int epoll_fd, conn *c) {
  if (c->request_len >= kRequestMax - 1) {
    close_conn(epoll_fd, c);
    return;
  }

  ssize_t n = read(c->fd, c->request + c->request_len, kRequestMax - 1 - c->request_len);
  if (n == 0) {
    close_conn(epoll_fd, c);
    return;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    close_conn(epoll_fd, c);
    return;
  }
  c->request_len += (size_t)n;
  c->request[c->request_len] = 0;

  char *end = strstr(c->request, "\r\n\r\n");
  if (!end) {
    if (c->request_len >= kRequestMax - 1) close_conn(epoll_fd, c);
    return;
  }

  // Only the request line matters here: "GET <path> HTTP/1.1".
  char method[8] = {0};
  char path[1024] = {0};
  if (sscanf(c->request, "%7s %1023s", method, path) != 2 || strcmp(method, "GET") != 0) {
    static const response bad = g_not_found;
    send_response(epoll_fd, c, &bad);
    return;
  }

  send_response(epoll_fd, c, route(path));
}

void write_response(int epoll_fd, conn *c) {
  while (c->out_off < c->out_len) {
    ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      close_conn(epoll_fd, c);
      return;
    }
    c->out_off += (size_t)n;
  }
  close_conn(epoll_fd, c);
}

} // namespace

int server_main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8787;

  g_index = make_response("text/html; charset=utf-8", (const char *)kIndexHtml, kIndexHtmlSize);
  static const char status_body[] =
      "{\"ok\":true,\"server\":\"padwave-cpp\",\"note\":\"bootstrap; the Elixir API is still the live one\"}";
  g_status = make_response("application/json", status_body, strlen(status_body));
  g_not_found = make_error(404, "Not Found", "not found\n");

  if (!g_index.data || !g_status.data || !g_not_found.data) {
    fprintf(stderr, "padwave serve: out of memory\n");
    return 1;
  }

  int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd < 0) {
    perror("socket");
    return 1;
  }
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  set_nonblocking(listen_fd);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(listen_fd);
    return 1;
  }
  if (listen(listen_fd, 64) < 0) {
    perror("listen");
    close(listen_fd);
    return 1;
  }

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigprocmask(SIG_BLOCK, &mask, nullptr);
  int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    perror("epoll_create1");
    return 1;
  }

  conn conns[kMaxConns];
  epoll_add(epoll_fd, listen_fd, EPOLLIN, nullptr);
  if (signal_fd >= 0) epoll_add(epoll_fd, signal_fd, EPOLLIN, (void *)1);

  fprintf(stderr, "padwave serve: http://0.0.0.0:%d (%zu byte UI)\n", port, kIndexHtmlSize);

  struct epoll_event events[64];
  bool running = true;

  while (running) {
    int ready = epoll_wait(epoll_fd, events, 64, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < ready; i++) {
      void *ptr = events[i].data.ptr;
      if (ptr == nullptr) {
        for (;;) {
          int fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (fd < 0) break;

          conn *slot = nullptr;
          for (int j = 0; j < kMaxConns; j++)
            if (conns[j].fd < 0) {
              slot = &conns[j];
              break;
            }
          if (!slot) {
            close(fd);
            continue;
          }
          slot->fd = fd;
          slot->request_len = 0;
          slot->out = nullptr;
          slot->out_len = 0;
          slot->out_off = 0;
          epoll_add(epoll_fd, fd, EPOLLIN, slot);
        }
        continue;
      }

      if (ptr == (void *)1) {
        struct signalfd_siginfo info;
        if (read(signal_fd, &info, sizeof(info)) == sizeof(info)) running = false;
        continue;
      }

      conn *c = (conn *)ptr;
      if (c->fd < 0) continue;

      if (events[i].events & EPOLLIN) read_request(epoll_fd, c);
      if (c->fd >= 0 && (events[i].events & EPOLLOUT)) write_response(epoll_fd, c);
      if (c->fd >= 0 && (events[i].events & (EPOLLERR | EPOLLHUP))) close_conn(epoll_fd, c);
    }
  }

  for (int i = 0; i < kMaxConns; i++) close_conn(epoll_fd, &conns[i]);
  close(epoll_fd);
  if (signal_fd >= 0) close(signal_fd);
  close(listen_fd);
  return 0;
}
