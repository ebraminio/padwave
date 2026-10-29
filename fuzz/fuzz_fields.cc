// Fuzz the 0x31 field-map parser: arbitrary text into a scratch OutputBody,
// once with the flags replaced and once merged.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "padwave_fields.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char first[4096];
  char second[4096];
  size_t n = size < sizeof(first) - 1 ? size : sizeof(first) - 1;
  memcpy(first, data, n);
  first[n] = 0;
  memcpy(second, first, n + 1);

  padwave::OutputBody body{};
  padwave_fields::apply_fields(&body, first);
  padwave_fields::apply_fields(&body, second);
  return 0;
}
