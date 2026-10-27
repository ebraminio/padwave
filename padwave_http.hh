// padwave_http.hh — the pure HTTP/1.1 request-head parser.
//
// The server reads into its own buffer and calls this; the fuzzer calls it with
// arbitrary bytes. It never assumes a NUL terminator and never reads past len.
#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct HttpRequest {
  char method[8];
  char path[1024];
  size_t head_len;  // through the final \r\n\r\n
  size_t body_len;  // Content-Length, or 0
  bool complete;    // the whole body is present
};

static constexpr size_t kHttpRequestMax = 8192;

// 1 = head parsed (check complete for the body), 0 = head not complete yet,
// -1 = malformed.
static inline int httpParseRequest(const char *buf, size_t len, HttpRequest *out) {
  if (!buf || !out) return -1;
  *out = HttpRequest{};

  size_t head_end = 0;
  bool found = false;
  for (size_t i = 0; i + 3 < len; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
      head_end = i + 4;
      found = true;
      break;
    }
  }
  if (!found) return 0;
  if (head_end > kHttpRequestMax) return -1;

  char head[kHttpRequestMax + 1];
  memcpy(head, buf, head_end);
  head[head_end] = 0;

  // A NUL inside the head would make sscanf/strcasestr see a shorter request.
  if (memchr(head, 0, head_end) != nullptr) return -1;
  if (sscanf(head, "%7s %1023s", out->method, out->path) != 2) return -1;

  size_t body = 0;
  const char *p = head;
  while ((p = strcasestr(p, "Content-Length:")) != nullptr) {
    p += 15;
    while (*p == ' ' || *p == '\t') p++;
    char *end = nullptr;
    errno = 0;
    unsigned long value = strtoul(p, &end, 10);
    if (end == p || errno != 0 || value > kHttpRequestMax) return -1;
    if (*end != '\r' && *end != ' ' && *end != '\t') return -1;
    body = (size_t)value;
    break;
  }
  if (head_end + body > kHttpRequestMax) return -1;

  out->head_len = head_end;
  out->body_len = body;
  out->complete = len >= head_end + body;
  return 1;
}
