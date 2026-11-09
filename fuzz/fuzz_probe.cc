// Fuzzes the probe path through the public C ABI: bytes -> source -> probe.
// Until T1.2/T1.5 land the source stub rejects everything, so this only
// exercises the plumbing; it becomes meaningful without changes.
#include <cstddef>
#include <cstdint>

#include "wt_api.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Tighter than the defaults, so the fuzzer spends its time in the parsers
  // rather than in large legitimate inputs.
  wt_limits limits;
  wt_limits_init_default(&limits);
  limits.max_dimension = 256;
  limits.max_frames = 64;
  limits.max_total_pixels = uint64_t{4} << 20;

  wt_source* source = nullptr;
  if (wt_source_create_memory(data, size, &limits, &source) != WT_OK) return 0;
  wt_info info;
  wt_probe(source, &info);
  wt_source_destroy(source);
  return 0;
}
