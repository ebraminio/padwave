// padwave serve — the C++ web front.
//
// A single epoll loop serving the panel from the binary and proxying /api/* to
// the standalone radio; while PADWAVE_STANDALONE is unset it can still
// proxy to a legacy backend. The proxy is streaming, so /api/events (SSE) works:
// bytes are relayed as they arrive, with a 64 KiB per-connection buffer and
// backpressure when the client cannot keep up.
//
//   padwave serve [port] [backend-port]      default 8787 8787
//
// The UI is #embed on compilers that have it (Clang); GCC 12 gets a generated
// padwave_ui.hh from install.sh. Either way the binary needs no files.
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
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "padwave_app.hh"
#include "padwave_common.hh"
#include "padwave_status.hh"

#if defined(__has_embed)
#if __has_embed("../ui/index.html")
static const unsigned char kIndexHtml[] = {
#embed "../ui/index.html"
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
constexpr size_t kRequestMax = 8192;
constexpr size_t kBufferMax = 65536;
constexpr int kMaxFds = 1024;

struct conn {
  int fd = -1;              // client
  int up = -1;              // backend, when proxying
  char in[kRequestMax];
  size_t in_len = 0;
  char up_req[kRequestMax];
  size_t up_len = 0;
  size_t up_off = 0;
  char out[kBufferMax];
  size_t out_len = 0;
  size_t out_off = 0;
  bool up_sent = false;
  bool static_reply = false;
  bool front_sse = false;
  bool status_sse = false;
  bool client_blocked = false;
};

conn g_conns[kMaxConns];
conn *g_owner[kMaxFds];

int g_epoll = -1;
int g_backend_port = 8787;
int g_status_fd = -1;
bool g_standalone = false;
int g_timer_fd = -1;
int64_t g_start_ms = 0;
unsigned long g_requests = 0;

struct player_status {
  bool valid = false;
  long pos = 0, dur = 0, sent = 0, underruns = 0;
  int paused = 0;
};

struct input_status {
  bool valid = false;
  int lx = 0, ly = 0, rx = 0, ry = 0, l2 = 0, r2 = 0, battery = 0, jack = 0;
  unsigned buttons = 0;
};

player_status g_player;
input_status g_input;

int64_t monotonic_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void parse_status(const char *line) {
  if (strncmp(line, "player ", 7) == 0) {
    if (sscanf(line + 7, "%ld %ld %ld %ld %d", &g_player.pos, &g_player.dur, &g_player.sent,
               &g_player.underruns, &g_player.paused) == 5)
      g_player.valid = true;
  } else if (strncmp(line, "input ", 6) == 0) {
    unsigned lx, ly, rx, ry, l2, r2, buttons;
    int battery, jack;
    if (sscanf(line + 6, "%u %u %u %u %u %u %u %d %d", &lx, &ly, &rx, &ry, &l2, &r2, &buttons,
               &battery, &jack) == 9) {
      g_input = input_status{};
      g_input.valid = true;
      g_input.lx = (int)lx;
      g_input.ly = (int)ly;
      g_input.rx = (int)rx;
      g_input.ry = (int)ry;
      g_input.l2 = (int)l2;
      g_input.r2 = (int)r2;
      g_input.buttons = buttons;
      g_input.battery = battery;
      g_input.jack = jack;
    }
  }
}
char g_index_response[70000];
size_t g_index_len = 0;

int make_static(char *dst, size_t cap, const char *type, const char *body, size_t body_len) {
  int n = snprintf(dst, cap,
                   "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                   "Connection: close\r\nCache-Control: no-cache\r\n\r\n",
                   type, body_len);
  if (n < 0 || (size_t)n + body_len > cap) return -1;
  memcpy(dst + n, body, body_len);
  return n + (int)body_len;
}

int front_json(char *buf, size_t cap);

void close_conn(conn *c) {
  if (c->fd >= 0) {
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, nullptr);
    if (c->fd < kMaxFds) g_owner[c->fd] = nullptr;
    close(c->fd);
  }
  if (c->up >= 0) {
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->up, nullptr);
    if (c->up < kMaxFds) g_owner[c->up] = nullptr;
    close(c->up);
  }
  *c = conn{};
}

bool add_fd(int fd, uint32_t events, conn *owner) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  if (epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev) != 0) return false;
  if (fd < kMaxFds) g_owner[fd] = owner;
  return true;
}

bool mod_fd(int fd, uint32_t events) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  return epoll_ctl(g_epoll, EPOLL_CTL_MOD, fd, &ev) == 0;
}

// Copy the request line and all headers except Connection/Keep-Alive, then a
// Connection: close, so the backend closes after the response (SSE stays open
// because the client detaches, not the backend).
size_t build_upstream_request(conn *c, const char *headers_end, size_t body_len) {
  char *dst = c->up_req;
  size_t cap = sizeof(c->up_req);
  size_t used = 0;

  const char *line_end = strstr(c->in, "\r\n");
  size_t line_len = line_end ? (size_t)(line_end - c->in) + 2 : 0;
  if (line_len == 0 || line_len > cap) return 0;
  memcpy(dst, c->in, line_len);
  used = line_len;

  const char *p = c->in + line_len;
  while (p < headers_end) {
    const char *eol = strstr(p, "\r\n");
    if (!eol || eol == p) break;
    size_t len = (size_t)(eol - p) + 2;
    bool drop = strncasecmp(p, "Connection:", 11) == 0 || strncasecmp(p, "Keep-Alive:", 11) == 0 ||
                strncasecmp(p, "Proxy-Connection:", 17) == 0;
    if (!drop) {
      if (used + len > cap) return 0;
      memcpy(dst + used, p, len);
      used += len;
    }
    p = eol + 2;
  }

  static const char close_hdr[] = "Connection: close\r\n\r\n";
  if (used + sizeof(close_hdr) - 1 + body_len > cap) return 0;
  memcpy(dst + used, close_hdr, sizeof(close_hdr) - 1);
  used += sizeof(close_hdr) - 1;
  if (body_len) {
    memcpy(dst + used, headers_end + 4, body_len);
    used += body_len;
  }
  return used;
}

void start_proxy(conn *c, const char *headers_end, size_t body_len) {
  c->up_len = build_upstream_request(c, headers_end, body_len);
  if (c->up_len == 0) {
    c->static_reply = true;
    c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                  "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n"
                                  "Connection: close\r\n\r\nbad request\n");
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  int up = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (up < 0) {
    close_conn(c);
    return;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)g_backend_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(up, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
    close(up);
    close_conn(c);
    return;
  }
  c->up = up;
  add_fd(up, EPOLLOUT, c);
}

void read_client(app *a, conn *c) {
  if (c->in_len >= kRequestMax - 1) {
    close_conn(c);
    return;
  }
  ssize_t n = read(c->fd, c->in + c->in_len, kRequestMax - 1 - c->in_len);
  if (n == 0) {
    close_conn(c);
    return;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    close_conn(c);
    return;
  }
  c->in_len += (size_t)n;
  c->in[c->in_len] = 0;

  char *end = strstr(c->in, "\r\n\r\n");
  if (!end) {
    if (c->in_len >= kRequestMax - 1) close_conn(c);
    return;
  }

  size_t head_len = (size_t)(end - c->in) + 4;
  size_t body_len = 0;
  const char *cl = strcasestr(c->in, "Content-Length:");
  if (cl && cl < end) body_len = (size_t)strtoul(cl + 15, nullptr, 10);
  if (head_len + body_len > kRequestMax) {
    close_conn(c);
    return;
  }
  if (c->in_len < head_len + body_len) return;  // body still arriving

  char method[8] = {0}, path[1024] = {0};
  if (sscanf(c->in, "%7s %1023s", method, path) != 2) {
    close_conn(c);
    return;
  }

  g_requests++;

  if (g_standalone && strcmp(path, "/api/status") == 0) {
    static char body[8192];
    int len = app_status_json(a, body, sizeof(body));
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/events") == 0) {
    c->status_sse = true;
    c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                  "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                                  "Connection: keep-alive\r\n\r\n");
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/library") == 0) {
    char *body = (char *)malloc(65536);
    int len = body ? app_library_json(a, body, 65536) : 0;
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body ? body : "{}",
                                     body ? (size_t)len : 2);
    free(body);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/config") == 0) {
    if (strcmp(method, "POST") == 0) {
      static const char ok[] = "{\"ok\":true}";
      c->static_reply = true;
      c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", ok, sizeof(ok) - 1);
      mod_fd(c->fd, EPOLLOUT);
      return;
    }
    char body[1024];
    int len = app_config_json(body, sizeof(body));
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/schema") == 0) {
    char bindings[512];
    app_bindings_json(bindings, sizeof(bindings));
    char body[1024];
    int len = snprintf(body, sizeof(body),
                       "{\"note\":\"wire formats live in include/padwave_schemas.hh\","
                       "\"bindings\":%s}",
                       bindings);
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strcmp(path, "/api/command") == 0) {
    if (strcmp(method, "POST") != 0) {
      c->static_reply = true;
      c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                    "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n"
                                    "Connection: close\r\n\r\n");
      mod_fd(c->fd, EPOLLOUT);
      return;
    }
    char result[512];
    int len = app_handle_command(a, c->in + head_len, body_len, result, sizeof(result));
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", result, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (g_standalone && strncmp(path, "/api/", 5) == 0 && strcmp(path, "/api/front") != 0 && strcmp(path, "/api/front/events") != 0) {
    static const char body[] = "{\"ok\":false,\"error\":\"not implemented\"}";
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body,
                                     sizeof(body) - 1);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (strcmp(path, "/api/front/events") == 0) {
    c->front_sse = true;
    c->out_len = (size_t)snprintf(c->out, sizeof(c->out),
                                  "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                                  "Connection: keep-alive\r\n\r\n");
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (strcmp(path, "/api/front") == 0) {
    char body[768];
    int len = front_json(body, sizeof(body));
    c->static_reply = true;
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body, (size_t)len);
    mod_fd(c->fd, EPOLLOUT);
    return;
  }

  if (strncmp(path, "/api/", 5) == 0) {
    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0 &&
        strcmp(method, "PUT") != 0 && strcmp(method, "DELETE") != 0) {
      close_conn(c);
      return;
    }
    start_proxy(c, end, body_len);
    return;
  }

  c->static_reply = true;
  if (strcmp(path, "/health") == 0) {
    static const char body[] = "{\"ok\":true}\n";
    c->out_len = (size_t)make_static(c->out, sizeof(c->out), "application/json", body,
                                     sizeof(body) - 1);
  } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
    if (g_index_len <= sizeof(c->out)) {
      memcpy(c->out, g_index_response, g_index_len);
      c->out_len = g_index_len;
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
  mod_fd(c->fd, EPOLLOUT);
}

void flush_client(conn *c) {
  while (c->out_off < c->out_len) {
    ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        c->client_blocked = true;
        mod_fd(c->fd, EPOLLOUT);
        return;
      }
      close_conn(c);
      return;
    }
    c->out_off += (size_t)n;
  }
  c->out_off = c->out_len = 0;
  c->client_blocked = false;

  if (c->front_sse || c->status_sse) {
    mod_fd(c->fd, 0);
    return;
  }

  if (c->static_reply) {
    close_conn(c);
    return;
  }
  // Proxy: let the backend send more.
  if (c->up >= 0) mod_fd(c->up, EPOLLIN);
}

void write_upstream(conn *c) {
  while (c->up_off < c->up_len) {
    ssize_t n = write(c->up, c->up_req + c->up_off, c->up_len - c->up_off);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        mod_fd(c->up, EPOLLOUT);
        return;
      }
      close_conn(c);
      return;
    }
    c->up_off += (size_t)n;
  }
  c->up_sent = true;
  mod_fd(c->up, EPOLLIN);
}

void read_upstream(conn *c) {
  if (c->client_blocked || c->out_len - c->out_off >= kBufferMax - 4096) {
    mod_fd(c->up, 0);
    return;
  }
  size_t space = kBufferMax - c->out_len;
  ssize_t n = read(c->up, c->out + c->out_len, space);
  if (n == 0) {
    // Backend finished. Flush what is buffered, then close both.
    c->up_sent = true;
    int up = c->up;
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, up, nullptr);
    if (up < kMaxFds) g_owner[up] = nullptr;
    close(up);
    c->up = -1;
    flush_client(c);
    if (c->fd >= 0) {
      while (c->out_off < c->out_len) {
        ssize_t w = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (w <= 0) break;
        c->out_off += (size_t)w;
      }
      close_conn(c);
    }
    return;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    close_conn(c);
    return;
  }
  c->out_len += (size_t)n;
  flush_client(c);
}

int front_json(char *buf, size_t cap) {
  char node[64];
  int controller = find_dualsense(node, sizeof(node)) >= 0 ? 1 : 0;
  return snprintf(buf, cap,
                  "{\"ok\":true,\"front\":1,\"uptime_s\":%ld,\"requests\":%lu,"
                  "\"controller\":%d,"
                  "\"player\":{\"valid\":%d,\"pos\":%ld,\"dur\":%ld,\"sent\":%ld,"
                  "\"underruns\":%ld,\"paused\":%d},"
                  "\"input\":{\"valid\":%d,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,"
                  "\"l2\":%d,\"r2\":%d,\"buttons\":%u,\"battery\":%d,\"jack\":%d}}",
                  (long)((monotonic_ms() - g_start_ms) / 1000), g_requests, controller,
                  g_player.valid ? 1 : 0, g_player.pos, g_player.dur, g_player.sent,
                  g_player.underruns, g_player.paused, g_input.valid ? 1 : 0, g_input.lx, g_input.ly, g_input.rx,
                  g_input.ry, g_input.l2, g_input.r2, g_input.buttons, g_input.battery,
                  g_input.jack);
}

void append_status_event(app *a, conn *c) {
  if (c->out_off > 0) {
    if (c->out_off < c->out_len) memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
    c->out_len -= c->out_off;
    c->out_off = 0;
  }
  char body[8192];
  int n = app_status_json(a, body, sizeof(body));
  if (n <= 0) return;
  char *chunk = (char *)malloc((size_t)n + 64);
  if (!chunk) return;
  int m = snprintf(chunk, (size_t)n + 64, "%x\r\ndata: ", n + 6);
  memcpy(chunk + m, body, (size_t)n);
  m += n;
  memcpy(chunk + m, "\n\n\r\n", 4);
  m += 4;
  if (c->out_len + (size_t)m <= sizeof(c->out)) {
    memcpy(c->out + c->out_len, chunk, (size_t)m);
    c->out_len += (size_t)m;
    mod_fd(c->fd, EPOLLOUT);
  }
  free(chunk);
}

void append_front_event(conn *c) {
  if (c->out_off > 0) {
    if (c->out_off < c->out_len)
      memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
    c->out_len -= c->out_off;
    c->out_off = 0;
  }

  char body[768];
  int n = front_json(body, sizeof(body));
  if (n <= 0) return;
  char event[800];
  int e = snprintf(event, sizeof(event), "data: %s\n\n", body);
  if (e <= 0) return;
  char chunk[832];
  int m = snprintf(chunk, sizeof(chunk), "%x\r\n", e);
  if (m <= 0 || (size_t)m + (size_t)e + 2 > sizeof(chunk)) return;
  memcpy(chunk + m, event, (size_t)e);
  m += e;
  memcpy(chunk + m, "\r\n", 2);
  m += 2;

  if (c->out_len + (size_t)m > sizeof(c->out)) return;
  memcpy(c->out + c->out_len, chunk, (size_t)m);
  c->out_len += (size_t)m;
  mod_fd(c->fd, EPOLLOUT);
}

} // namespace

int server_main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8787;
  g_backend_port = argc > 2 ? atoi(argv[2]) : 8787;

  int n = snprintf(nullptr, 0,
                   "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                   "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",
                   kIndexHtmlSize);
  if ((size_t)n + kIndexHtmlSize >= sizeof(g_index_response)) {
    fprintf(stderr, "padwave serve: UI too large for the static buffer\n");
    return 1;
  }
  snprintf(g_index_response, sizeof(g_index_response),
           "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
           "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",
           kIndexHtmlSize);
  memcpy(g_index_response + n, kIndexHtml, kIndexHtmlSize);
  g_index_len = (size_t)n + kIndexHtmlSize;

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

  g_standalone = getenv("PADWAVE_STANDALONE") != nullptr;
  app *a = nullptr;
  if (g_standalone) a = app_create();

  g_start_ms = monotonic_ms();

  g_status_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (g_status_fd >= 0) {
    struct sockaddr_un status_addr;
    memset(&status_addr, 0, sizeof(status_addr));
    status_addr.sun_family = AF_UNIX;
    snprintf(status_addr.sun_path, sizeof(status_addr.sun_path), "%s", padwave_status::kPath);
    unlink(padwave_status::kPath);
    if (bind(g_status_fd, (struct sockaddr *)&status_addr, sizeof(status_addr)) != 0) {
      close(g_status_fd);
      g_status_fd = -1;
    }
  }

  g_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (g_timer_fd >= 0) {
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_interval.tv_nsec = 500000000;
    its.it_value = its.it_interval;
    timerfd_settime(g_timer_fd, 0, &its, nullptr);
  }

  g_epoll = epoll_create1(EPOLL_CLOEXEC);
  if (g_epoll < 0) {
    perror("epoll_create1");
    return 1;
  }
  add_fd(listen_fd, EPOLLIN, nullptr);
  if (signal_fd >= 0) add_fd(signal_fd, EPOLLIN, nullptr);
  if (g_status_fd >= 0) add_fd(g_status_fd, EPOLLIN, nullptr);
  if (g_timer_fd >= 0) add_fd(g_timer_fd, EPOLLIN, nullptr);
  if (g_standalone) {
    int fds[4];
    int n = app_fds(a, fds, 4);
    for (int i = 0; i < n; i++) add_fd(fds[i], EPOLLIN, nullptr);
  }

  fprintf(stderr, "padwave serve: http://0.0.0.0:%d (%zu byte UI)\n", port, kIndexHtmlSize);

  struct epoll_event events[64];
  bool running = true;
  while (running) {
    int ready = epoll_wait(g_epoll, events, 64, -1);
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
          conn *slot = nullptr;
          for (int j = 0; j < kMaxConns; j++)
            if (g_conns[j].fd < 0) {
              slot = &g_conns[j];
              break;
            }
          if (!slot) {
            close(client);
            continue;
          }
          *slot = conn{};
          slot->fd = client;
          add_fd(client, EPOLLIN, slot);
        }
        continue;
      }
      if (fd == g_timer_fd) {
        uint64_t expirations = 0;
        if (read(g_timer_fd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
          if (g_standalone) {
            app_tick(a);
            int fds[4];
            int n = app_fds(a, fds, 4);
            for (int i = 0; i < n; i++) add_fd(fds[i], EPOLLIN, nullptr);
          }
          for (int j = 0; j < kMaxConns; j++) {
            if (g_conns[j].fd < 0) continue;
            if (g_conns[j].front_sse) append_front_event(&g_conns[j]);
            if (g_conns[j].status_sse) append_status_event(a, &g_conns[j]);
          }
        }
        continue;
      }
      if (fd == g_status_fd) {
        char line[256];
        ssize_t n = recv(g_status_fd, line, sizeof(line) - 1, 0);
        if (n > 0) {
          line[n] = 0;
          parse_status(line);
        }
        continue;
      }
      if (fd == signal_fd) {
        struct signalfd_siginfo info;
        if (read(signal_fd, &info, sizeof(info)) == sizeof(info)) running = false;
        continue;
      }

      if (g_standalone && app_owns_fd(a, fd)) {
        app_handle_fd(a, fd);
        continue;
      }

      conn *c = (fd >= 0 && fd < kMaxFds) ? g_owner[fd] : nullptr;
      if (!c) continue;

      if (fd == c->fd) {
        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          close_conn(c);
          continue;
        }
        if (events[i].events & EPOLLIN) read_client(a, c);
        if (c->fd >= 0 && (events[i].events & EPOLLOUT)) flush_client(c);
      } else if (fd == c->up) {
        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          close_conn(c);
          continue;
        }
        if (events[i].events & EPOLLOUT) write_upstream(c);
        if (c->up >= 0 && (events[i].events & EPOLLIN)) read_upstream(c);
      }
    }
  }

  for (int i = 0; i < kMaxConns; i++) close_conn(&g_conns[i]);
  if (g_status_fd >= 0) {
    close(g_status_fd);
    unlink(padwave_status::kPath);
  }
  if (g_timer_fd >= 0) close(g_timer_fd);
  if (a) app_destroy(a);
  close(g_epoll);
  if (signal_fd >= 0) close(signal_fd);
  close(listen_fd);
  return 0;
}
