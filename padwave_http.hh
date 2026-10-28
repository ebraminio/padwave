// padwave_http.hh — the HTTP/1.1 request-head parser as an HttpRequest.
//
// Construction parses: HttpRequest req(buf, len). The parser never assumes a
// NUL terminator and never reads past len. It is used by the server and by the
// HTTP fuzzer.
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
  enum class Status : uint8_t {
    Malformed,  // not a request head
    Incomplete, // no CRLFCRLF yet
    Parsed,     // head parsed; check complete for the body
  };

  static constexpr size_t kMax = 8192;

  char method[8] = {};
  char path[1024] = {};
  size_t head_len = 0;   // through the final \r\n\r\n
  size_t body_len = 0;   // Content-Length, or 0
  bool complete = false; // the whole body is present
  Status status = Status::Malformed;

  HttpRequest() = default;

  explicit HttpRequest(const char *buf, size_t len) { parse(buf, len); }

  bool valid() const { return status != Status::Malformed; }

  bool headReady() const { return status == Status::Parsed; }

private:
  void parse(const char *buf, size_t len) {
    if (!buf) return;

    size_t head_end = 0;
    bool found = false;
    for (size_t i = 0; i + 3 < len; i++) {
      if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
        head_end = i + 4;
        found = true;
        break;
      }
    }
    if (!found) {
      status = Status::Incomplete;
      return;
    }
    if (head_end > kMax) return;

    char head[kMax + 1];
    memcpy(head, buf, head_end);
    head[head_end] = 0;

    // A NUL inside the head would make sscanf/strcasestr see a shorter request.
    if (memchr(head, 0, head_end) != nullptr) return;
    if (sscanf(head, "%7s %1023s", method, path) != 2) return;

    size_t body = 0;
    const char *p = head;
    while ((p = strcasestr(p, "Content-Length:")) != nullptr) {
      p += 15;
      while (*p == ' ' || *p == '\t') p++;
      char *end = nullptr;
      errno = 0;
      unsigned long value = strtoul(p, &end, 10);
      if (end == p || errno != 0 || value > kMax) return;
      if (*end != '\r' && *end != ' ' && *end != '\t') return;
      body = static_cast<size_t>(value);
      break;
    }
    if (head_end + body > kMax) return;

    head_len = head_end;
    body_len = body;
    complete = len >= head_end + body;
    status = Status::Parsed;
  }
};
