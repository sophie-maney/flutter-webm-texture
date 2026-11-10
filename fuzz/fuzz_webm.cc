// Fuzzes the WebM demuxer (src/demux/webm.*) and, for inputs it accepts,
// the decode path behind wt_anim (T1.4): VP8/VP9 colour + alpha decode,
// scaling to a bucket and conversion into a caller buffer.
//
// Besides the sanitizers, it checks the index invariants that the decoder
// relies on and traps when one breaks, so libFuzzer reports it as a crash.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "demux/webm.h"
#include "wt_api.h"

namespace {

bool InBounds(const wt::Span& span, size_t size) {
  return span.offset <= size && span.size <= size - span.offset;
}

// Decodes up to max_frames frames at width x height (0 x 0: source size)
// into a buffer with row padding, then loops once with seek(0). Traps on a
// status the API doesn't allow or a write into the padding.
void Decode(wt_source* source, uint32_t width, uint32_t height,
            int max_frames) {
  wt_anim* anim = nullptr;
  if (wt_anim_create(source, width, height, &anim) != WT_OK) return;
  uint32_t w = 0, h = 0;
  wt_info info;
  if (wt_anim_get_info(anim, &info, &w, &h) != WT_OK || w == 0 || h == 0 ||
      info.frame_count == 0) {
    __builtin_trap();
  }
  const size_t row = size_t{w} * 4, stride = row + 16;
  std::vector<uint8_t> buffer(stride * h, 0xEE);
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = 0; i < max_frames; ++i) {
      wt_frame_info frame;
      const wt_status s = wt_anim_next_frame(
          anim, buffer.data(), buffer.size(), static_cast<uint32_t>(stride),
          (i & 1) ? WT_PIXEL_FORMAT_BGRA8888 : WT_PIXEL_FORMAT_RGBA8888,
          &frame);
      if (s == WT_END) break;
      if (s == WT_ERR_INVALID_ARGUMENT || s == WT_ERR_INTERNAL ||
          s == WT_ERR_UNIMPLEMENTED || (s == WT_OK && frame.duration_ms == 0)) {
        __builtin_trap();
      }
    }
    if (wt_anim_seek(anim, 0) != WT_OK) __builtin_trap();
  }
  for (uint32_t y = 0; y < h; ++y) {
    for (size_t x = row; x < stride; ++x) {
      if (buffer[y * stride + x] != 0xEE) __builtin_trap();
    }
  }
  wt_anim_destroy(anim);
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
      uint64_t{index.video.width} * index.video.height * index.frames.size() >
          limits.max_total_pixels) {
    __builtin_trap();
  }
  if (!index.frames.empty() &&
      (index.video.width == 0 || index.video.height == 0 ||
       index.video.width > limits.max_dimension ||
       index.video.height > limits.max_dimension)) {
    __builtin_trap();
  }

  // Decode only what the demuxer accepted, under limits tight enough that
  // the fuzzer spends its time on hostile bitstreams, not large frames.
  if (index.frames.empty() || index.video.width > 256 ||
      index.video.height > 256) {
    return 0;
  }
  wt_limits tight = limits;
  tight.max_dimension = 256;
  tight.max_frames = 64;
  wt_source* source = nullptr;
  if (wt_source_create_memory(data, size, &tight, &source) != WT_OK) return 0;
  Decode(source, 0, 0, 8);
  Decode(source, 48, 32, 4);
  wt_source_destroy(source);
  return 0;
}
