// Fuzz a sequence of 0x31 reports through one Input, so the d-pad/PS/jack edge
// state carries across reports. The custom mutator re-signs every 78-byte
// report whose first byte says 0x31.
#include <stddef.h>
#include <stdint.h>

extern "C" size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);

#include "padwave_app_stub.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Input h;
  constexpr size_t kReport = dualsense::InputReport::Size;
  for (size_t off = 0; off + kReport <= size; off += kReport) h.parse(data + off, kReport);
  return 0;
}

extern "C" size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                                          unsigned int seed) {
  size = LLVMFuzzerMutate(data, size, max_size);
  constexpr size_t kReport = dualsense::InputReport::Size;
  for (size_t off = 0; off + kReport <= size; off += kReport) {
    if (data[off] == dualsense::InputReport::ReportId) {
      auto *report = reinterpret_cast<dualsense::InputReport *>(data + off);
      report->sign();
    }
  }
  return size;
}
