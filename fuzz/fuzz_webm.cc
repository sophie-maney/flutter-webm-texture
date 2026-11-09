// Fuzzes the WebM demuxer (src/demux/webm.*) on its own: bytes -> frame
// index. T1.7 extends this target to decode the first frames of the index.
//
// Besides the sanitizers, it checks the index invariants that the decoder
// will rely on and traps when one breaks, so libFuzzer reports it as a crash.
#include <cstddef>
#include <cstdint>

#include "demux/webm.h"
#include "wt_api.h"

namespace {

bool InBounds(const wt::Span& span, size_t size) {
  return span.offset <= size && span.size <= size - span.offset;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  wt_limits limits;
  wt_limits_init_default(&limits);

  wt::WebmIndex index;
  const wt_status status =
      wt::DemuxWebm(wt::ByteView{data, size}, limits, &index);
  if (status != index.status || index.error == nullptr) __builtin_trap();
  if (status == WT_OK && (!index.has_video || index.frames.empty())) {
    __builtin_trap();
  }
  if (status != WT_OK && status != WT_ERR_TRUNCATED && !index.frames.empty()) {
    __builtin_trap();
  }
  if (index.frames.size() > limits.max_frames) __builtin_trap();
  for (const wt::WebmFrame& f : index.frames) {
    if (f.data.empty() || !InBounds(f.data, size)) __builtin_trap();
    if (!InBounds(f.alpha, size)) __builtin_trap();
    if (!f.alpha.empty() && !index.video.has_alpha()) __builtin_trap();
    if (f.duration_ns <= 0) __builtin_trap();
  }
  if (!index.frames.empty() &&
      (index.video.width == 0 || index.video.height == 0 ||
       index.video.width > limits.max_dimension ||
       index.video.height > limits.max_dimension)) {
    __builtin_trap();
  }
  return 0;
}
