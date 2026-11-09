// Fuzzes the probe path through the public C ABI: bytes -> source -> probe.
// Whatever probes OK must be inside every limit, and a failed probe must
// leave *info zeroed (T1.5).
#include <cstddef>
#include <cstdint>
#include <cstring>

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
  const wt_status status = wt_probe(source, &info);
  if (status != WT_OK) {
    wt_info zero;
    std::memset(&zero, 0, sizeof(zero));
    if (std::memcmp(&info, &zero, sizeof(info)) != 0) __builtin_trap();
  } else if (info.width == 0 || info.height == 0 || info.frame_count == 0 ||
             info.width > limits.max_dimension ||
             info.height > limits.max_dimension ||
             info.frame_count > limits.max_frames ||
             info.duration_ms > limits.max_duration_ms ||
             uint64_t{info.width} * info.height * info.frame_count >
                 limits.max_total_pixels) {
    __builtin_trap();
  }
  wt_source_destroy(source);
  return 0;
}
