// padwave — one translation unit: the app, its player and input engines, and
// the web server. The wire formats live in padwave_wire.hh.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "padwave_app.hh"
#include "padwave_http.hh"

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

namespace padwave_ui {
static const unsigned char kIndexHtml[] = {
#embed "index.html"
};
static const size_t kIndexHtmlSize = sizeof(kIndexHtml);
} // namespace padwave_ui

struct Server {
private:
  static constexpr int kMaxConns = 64;
  static constexpr size_t kRequestMax = 8192;
  static constexpr size_t kBufferMax = 65536;
  static constexpr int kMaxFds = 1024;

  struct Conn {
    int fd = -1; // client
    char in[kRequestMax];
    size_t in_len = 0;
    char out[kBufferMax];
    size_t out_len = 0;
    size_t out_off = 0;
    bool static_reply = false;
    bool front_sse = false;
    bool status_sse = false;
    bool client_blocked = false;
  };

  Conn conns[kMaxConns];
  Conn *owners[kMaxFds];
  int epoll_fd = -1;
  int timer_fd = -1;
  int64_t start_ms = 0;
  unsigned long requests = 0;
  char index_response[70000];
  size_t index_len = 0;
  App *app = nullptr;

public:
  int run(int port) {
    App a{}; // the app stays on the stack; the connection table is static in server_main

    int n = snprintf(nullptr, 0,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",
                     padwave_ui::kIndexHtmlSize);
    if ((size_t)n + padwave_ui::kIndexHtmlSize >= sizeof(index_response)) {
      fprintf(stderr, "padwave: UI too large for the static buffer\n");
      return 1;
    }
    snprintf(index_response, sizeof(index_response),
             "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",
             padwave_ui::kIndexHtmlSize);
    memcpy(index_response + n, padwave_ui::kIndexHtml, padwave_ui::kIndexHtmlSize);
    index_len = (size_t)n + padwave_ui::kIndexHtmlSize;

    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
      perror("socket");
      return 1;
    }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

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

    if (!a.init()) return 1;
    app = &a;
    start_ms = monotonicMs();

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd >= 0) {
      struct itimerspec its;
      memset(&its, 0, sizeof(its));
      its.it_interval.tv_nsec = 500000000;
      its.it_value = its.it_interval;
      timerfd_settime(timer_fd, 0, &its, nullptr);
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
      perror("epoll_create1");
      return 1;
    }
    addFd(listen_fd, EPOLLIN, nullptr);
    if (signal_fd >= 0) addFd(signal_fd, EPOLLIN, nullptr);
    if (timer_fd >= 0) addFd(timer_fd, EPOLLIN, nullptr);
    int app_fds[4];
    int app_fd_count = app->fds(app_fds, 4);
    for (int i = 0; i < app_fd_count; i++) addFd(app_fds[i], EPOLLIN, nullptr);

    fprintf(stderr, "padwave: http://0.0.0.0:%d (%zu byte UI)\n", port, padwave_ui::kIndexHtmlSize);

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
        int fd = events[i].data.fd;
        if (fd == listen_fd) {
          int listener = fd;
          for (;;) {
            int client = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client < 0) break;
            Conn *slot = nullptr;
            for (int j = 0; j < kMaxConns; j++)
              if (conns[j].fd < 0) {
                slot = &conns[j];
                break;
              }
            if (!slot) {
              close(client);
              continue;
            }
            *slot = Conn{};
            slot->fd = client;
            addFd(client, EPOLLIN, slot);
          }
          continue;
        }
        if (fd == timer_fd) {
          uint64_t expirations = 0;
          if (read(timer_fd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
            app->tick();
            int app_fds[4];
            int app_fd_count = app->fds(app_fds, 4);
            for (int i = 0; i < app_fd_count; i++) addFd(app_fds[i], EPOLLIN, nullptr);
            for (int j = 0; j < kMaxConns; j++) {
              if (conns[j].fd < 0) continue;
              if (conns[j].front_sse) appendFrontEvent(&conns[j]);
              if (conns[j].status_sse) appendStatusEvent(&conns[j]);
            }
          }
          continue;
        }
        if (fd == signal_fd) {
          struct signalfd_siginfo info;
          if (read(signal_fd, &info, sizeof(info)) == sizeof(info)) running = false;
          continue;
        }

        if (app->ownsFd(fd)) {
          app->handleFd(fd);
          continue;
        }

        Conn *c = (fd >= 0 && fd < kMaxFds) ? owners[fd] : nullptr;
        if (!c || fd != c->fd) continue;

        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          closeConn(c);
          continue;
        }
        if (events[i].events & EPOLLIN) readClient(c);
        if (c->fd >= 0 && (events[i].events & EPOLLOUT)) flushClient(c);
      }
    }

    for (int i = 0; i < kMaxConns; i++) closeConn(&conns[i]);
    if (timer_fd >= 0) close(timer_fd);
    if (app) app->shutdown();
    close(epoll_fd);
    if (signal_fd >= 0) close(signal_fd);
    close(listen_fd);
    return 0;
  }

private:
  static int bindingsText(char *buf, size_t cap) {
    static const char text[] = "bindings.circle=play\n"
                               "bindings.create=previous\n"
                               "bindings.fn1=toggle_pause\n"
                               "bindings.fn2=toggle_mute\n"
                               "bindings.l1=previous\n"
                               "bindings.l3=next_random\n"
                               "bindings.left_paddle=previous\n"
                               "bindings.mute=toggle_mute\n"
                               "bindings.options=next_random\n"
                               "bindings.r1=next_random\n"
                               "bindings.r3=previous\n"
                               "bindings.right_paddle=next_random\n"
                               "bindings.square=shuffle_game\n"
                               "bindings.touchpad=toggle_mute\n"
                               "bindings.triangle=toggle_pause\n";
    size_t n = strlen(text);
    if (n >= cap) n = cap ? cap - 1 : 0;
    if (cap) {
      memcpy(buf, text, n);
      buf[n] = 0;
    }
    return (int)n;
  }

  static int64_t monotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  }

  static int makeStatic(char *dst, size_t cap, const char *type, const char *body,
                        size_t body_len) {
    int n = snprintf(dst, cap,
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\nCache-Control: no-cache\r\n\r\n",
                     type, body_len);
    if (n < 0 || (size_t)n + body_len > cap) return -1;
    memcpy(dst + n, body, body_len);
    return n + (int)body_len;
  }

  void closeConn(Conn *c) {
    if (c->fd >= 0) {
      epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, nullptr);
      if (c->fd < kMaxFds) owners[c->fd] = nullptr;
      close(c->fd);
    }
    *c = Conn{};
  }

  bool addFd(int fd, uint32_t events, Conn *owner) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
    if (fd < kMaxFds) owners[fd] = owner;
    return true;
  }

  bool modFd(int fd, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
  }

  void readClient(Conn *c) {
    if (c->in_len >= kRequestMax - 1) {
      closeConn(c);
      return;
    }
    ssize_t n = read(c->fd, c->in + c->in_len, kRequestMax - 1 - c->in_len);
    if (n == 0) {
      closeConn(c);
      return;
    }
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      closeConn(c);
      return;
    }
    c->in_len += (size_t)n;
    c->in[c->in_len] = 0;

    HttpRequest req(c->in, c->in_len);
    if (!req.valid()) {
      closeConn(c);
      return;
    }
    if (!req.headReady()) {
      if (c->in_len >= kRequestMax - 1) closeConn(c);
      return;
    }
    if (!req.complete) return; // body still arriving

    const char *method = req.method;
    const char *path = req.path;
    size_t head_len = req.head_len;
    size_t body_len = req.body_len;

    requests++;

    if (strcmp(path, "/api/status") == 0) {
      static char body[8192];
      int len = app->statusText(body, sizeof(body));
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", body,
                                      (size_t)len);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/events") == 0) {
      c->status_sse = true;
      c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                    "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                                    "Connection: keep-alive\r\n\r\n");
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/library") == 0) {
      char *body = (char *)malloc(65536);
      int len = body ? app->libraryText(body, 65536) : 0;
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8",
                                      body ? body : "", body ? (size_t)len : 0);
      free(body);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/config") == 0) {
      if (strcmp(method, "POST") == 0) {
        app->configSet(c->in + head_len);
        static const char ok[] = "ok=true\n";
        c->static_reply = true;
        c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", ok,
                                        sizeof(ok) - 1);
        modFd(c->fd, EPOLLOUT);
        return;
      }
      char body[1024];
      int len = app->configText(body, sizeof(body));
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", body,
                                      (size_t)len);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/schema") == 0) {
      char body[1024];
      int len = bindingsText(body, sizeof(body));
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", body,
                                      (size_t)len);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/command") == 0) {
      if (strcmp(method, "POST") != 0) {
        c->static_reply = true;
        c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                      "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n"
                                      "Connection: close\r\n\r\n");
        modFd(c->fd, EPOLLOUT);
        return;
      }
      char result[512];
      int len = app->handleCommand(c->in + head_len, body_len, result, sizeof(result));
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", result,
                                      (size_t)len);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/output") == 0) {
      if (strcmp(method, "POST") != 0) {
        c->static_reply = true;
        c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                      "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n"
                                      "Connection: close\r\n\r\n");
        modFd(c->fd, EPOLLOUT);
        return;
      }
      char result[256];
      int len = app->output(c->in + head_len, result, sizeof(result));
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", result,
                                      (size_t)len);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/debug/trigger") == 0) {
      char result[256];
      int len = app->debugTrigger(c->in + head_len, result, sizeof(result));
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", result,
                                      (size_t)len);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/mic/reset") == 0) {
      app->micReset();
      static const char body[] = "ok=true\n";
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", body,
                                      sizeof(body) - 1);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/reload") == 0) {
      static const char body[] = "ok=true\nreloaded=0\n";
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", body,
                                      sizeof(body) - 1);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strncmp(path, "/api/", 5) == 0 && strcmp(path, "/api/front") != 0 &&
        strcmp(path, "/api/front/events") != 0) {
      static const char body[] = "ok=false\nerror=not implemented\n";
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", body,
                                      sizeof(body) - 1);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/front/events") == 0) {
      c->front_sse = true;
      c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                    "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                                    "Connection: keep-alive\r\n\r\n");
      modFd(c->fd, EPOLLOUT);
      return;
    }

    if (strcmp(path, "/api/front") == 0) {
      char body[1024];
      int len = frontText(body, sizeof(body));
      c->static_reply = true;
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", body,
                                      (size_t)len);
      modFd(c->fd, EPOLLOUT);
      return;
    }

    c->static_reply = true;
    if (strcmp(path, "/health") == 0) {
      static const char body[] = "ok=true\n";
      c->out_len = (size_t)makeStatic(c->out, sizeof(c->out), "text/plain; charset=utf-8", body,
                                      sizeof(body) - 1);
    } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
      if (index_len <= sizeof(c->out)) {
        memcpy(c->out, index_response, index_len);
        c->out_len = index_len;
      } else {
        c->out_len = 0;
      }
    } else {
      static const char body[] = "not found\n";
      int n = snprintf(c->out, sizeof(c->out),
                       "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                       "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                       sizeof(body) - 1, body);
      c->out_len = (size_t)n;
    }
    modFd(c->fd, EPOLLOUT);
  }

  void flushClient(Conn *c) {
    while (c->out_off < c->out_len) {
      ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          c->client_blocked = true;
          modFd(c->fd, EPOLLOUT);
          return;
        }
        closeConn(c);
        return;
      }
      c->out_off += (size_t)n;
    }
    c->out_off = c->out_len = 0;
    c->client_blocked = false;

    if (c->front_sse || c->status_sse) {
      modFd(c->fd, 0);
      return;
    }

    if (c->static_reply) closeConn(c);
  }

  int frontText(char *buf, size_t cap) {
    return app->frontText(buf, cap, requests, (long)((monotonicMs() - start_ms) / 1000));
  }

  void appendSse(Conn *c, const char *body, size_t n) {
    if (c->out_off > 0) {
      if (c->out_off < c->out_len) memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
      c->out_len -= c->out_off;
      c->out_off = 0;
    }
    if (n == 0) return;

    // Every key=value line needs its own data: field; a line without one is
    // dropped by the event source. Count the exact payload, then write it.
    size_t payload = 1; // the blank line that ends the event
    for (size_t pos = 0; pos < n;) {
      size_t end = pos;
      while (end < n && body[end] != '\n') end++;
      payload += 6 + (end - pos) + 1;
      pos = end < n ? end + 1 : end;
    }

    char *chunk = (char *)malloc(payload + 48);
    if (!chunk) return;

    size_t pos = 0;
    int m = snprintf(chunk, 48, "%zx\r\n", payload);
    size_t o = (m > 0) ? (size_t)m : 0;

    while (pos < n) {
      size_t end = pos;
      while (end < n && body[end] != '\n') end++;
      memcpy(chunk + o, "data: ", 6);
      o += 6;
      memcpy(chunk + o, body + pos, end - pos);
      o += end - pos;
      chunk[o++] = '\n';
      pos = end < n ? end + 1 : end;
    }
    chunk[o++] = '\n';
    memcpy(chunk + o, "\r\n", 2);
    o += 2;

    if (c->out_len + o <= sizeof(c->out)) {
      memcpy(c->out + c->out_len, chunk, o);
      c->out_len += o;
      modFd(c->fd, EPOLLOUT);
    }
    free(chunk);
  }

  void appendStatusEvent(Conn *c) {
    char body[8192];
    int n = app->statusText(body, sizeof(body));
    if (n > 0) appendSse(c, body, (size_t)n);
  }

  void appendFrontEvent(Conn *c) {
    char body[1024];
    int n = frontText(body, sizeof(body));
    if (n > 0) appendSse(c, body, (size_t)n);
  }
};

int server_main(int argc, char **argv) {
  static Server server{}; // connection table in BSS; the App stays on the stack
  int port = argc > 1 ? atoi(argv[1]) : 8787;
  return server.run(port);
}

// padwave — one binary, one process, one thread.
//
//   padwave [port]    the app: panel, radio, player and input decoder
//
// All of it is in this translation unit; padwave_app.hh, padwave_player.hh,
// padwave_input.hh and padwave_http.hh describe the parts. The wire format is
// in padwave_wire.hh, and every part uses the same schema.
int main(int argc, char **argv) { return server_main(argc, argv); }
