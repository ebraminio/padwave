// Fuzz the HTTP request-head parser with arbitrary bytes.
#include <stddef.h>
#include <stdint.h>

#include "padwave_http.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  HttpRequest req;
  httpParseRequest(reinterpret_cast<const char *>(data), size, &req);
  return 0;
}
