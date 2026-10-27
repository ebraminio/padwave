// Fuzz the controller input engine: arbitrary bytes go through the same parser
// the hidraw fd feeds, but without a device (Input::parse). The custom mutator
// keeps the 0x31 CRC valid so mutations reach the body, not just the validity
// gate; 0x01 (USB) has no CRC and an arbitrary short input can still exercise
// the mic path.
#include <stddef.h>
#include <stdint.h>

extern "C" size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);

// input_create() only needs discovery; the fuzzer never opens a device.
static inline int find_dualsense(char *node, size_t len) {
  (void)node;
  (void)len;
  return -1;
}
static inline int find_dualsense() { return -1; }

#include "padwave_input.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Input h;
  h.parse(data, size);
  return 0;
}

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned int seed) {
  size = LLVMFuzzerMutate(data, size, max_size);

  if (size == padwave::InputReport::Size && data[0] == padwave::InputReport::ReportId) {
    auto *report = reinterpret_cast<padwave::InputReport *>(data);
    report->sign();  // keep the CRC in sync with the mutated body
  }
  return size;
}
