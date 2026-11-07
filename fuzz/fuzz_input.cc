// Fuzz the controller input engine: arbitrary bytes go through the same parser
// the hidraw fd feeds, but without a device (Input::parse). The custom mutator
// keeps the 0x31 CRC valid so mutations reach the body, not just the validity
// gate; 0x01 (USB) has no CRC and an arbitrary short input can still exercise
// the mic path.
#include <stddef.h>
#include <stdint.h>

extern "C" size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);

#include "padwave_app_stub.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Input h;
  h.parse(data, size);
  return 0;
}

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned int seed) {
  size = LLVMFuzzerMutate(data, size, max_size);

  if (size == sizeof(dualsense::InputReport)) {
    auto *report = reinterpret_cast<dualsense::InputReport *>(data);
    report->sign(); // keep the CRC in sync with the mutated body
  }
  return size;
}
