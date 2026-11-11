// Fuzzes the WebP path (T5.1): the demuxer (src/demux/webp.*) and, for inputs
// it accepts, wt_anim decoding through WebPAnimDecoder, RgbaToYuva, scaling
// and conversion into a caller buffer.
//
// Each accepted input is decoded at a small bucket and, with tight limits,
// at source size (0 x 0); both passes seek into the middle and back to 0 and
// skip frames, so the reset-and-replay path runs too. The build's sanitizers
// do most of the work; the invariants below trap on a broken index.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "api/handles.h"
#include "demux/webp.h"
#include "wt_api.h"

namespace {

// Plays up to max_frames frames at width x height (0 x 0: source size) into a
// buffer with row padding, seeking about. Traps on a status the API doesn't
// allow or a write into the padding.
void Decode(wt_source* source, uint32_t width, uint32_t height,
            uint32_t max_frames) {
  wt_anim* anim = nullptr;
  if (wt_anim_create(source, width, height, &anim) != WT_OK) return;
  uint32_t w = 0, h = 0;
  wt_info info;
  if (wt_anim_get_info(anim, &info, &w, &h) != WT_OK || w == 0 || h == 0 ||
      info.frame_count == 0 || info.format != WT_FORMAT_WEBP) {
    __builtin_trap();
  }
  const size_t row = size_t{w} * 4, stride = row + 16;
  std::vector<uint8_t> buffer(stride * h, 0xEE);
  const uint32_t seeks[] = {0, info.frame_count / 2, 0, info.frame_count - 1};
  for (uint32_t pass = 0; pass < 4; ++pass) {
    if (wt_anim_seek(anim, seeks[pass]) != WT_OK) __builtin_trap();
    for (uint32_t i = 0; i < max_frames; ++i) {
      wt_frame_info frame;
      const wt_status s =
          (i & 1) && pass >= 2
              ? anim->anim->SkipFrame(&frame)
              : wt_anim_next_frame(anim, buffer.data(), buffer.size(),
                                   static_cast<uint32_t>(stride),
                                   (i & 1) ? WT_PIXEL_FORMAT_BGRA8888
                                           : WT_PIXEL_FORMAT_RGBA8888,
                                   &frame);
      if (s == WT_END) break;
      if (s == WT_ERR_INVALID_ARGUMENT || s == WT_ERR_INTERNAL ||
          s == WT_ERR_UNIMPLEMENTED || (s == WT_OK && frame.duration_ms == 0)) {
        __builtin_trap();
      }
    }
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
  // Tighter than the defaults, so the fuzzer spends its time in the parsers
  // rather than in large legitimate inputs.
  wt_limits limits;
  wt_limits_init_default(&limits);
  limits.max_dimension = 256;
  limits.max_frames = 32;
  limits.max_total_pixels = uint64_t{2} << 20;

  wt::WebpIndex index;
  const wt_status status =
      wt::DemuxWebp(wt::ByteView{data, size}, limits, &index);
  if (status != WT_OK) {
    if (!index.duration_ms.empty() || !index.prefix.empty()) __builtin_trap();
    return 0;
  }
  if (index.duration_ms.empty() ||
      index.duration_ms.size() > limits.max_frames || index.width == 0 ||
      index.height == 0 || index.width > limits.max_dimension ||
      index.height > limits.max_dimension ||
      uint64_t{index.width} * index.height * index.duration_ms.size() >
          limits.max_total_pixels ||
      index.prefix.empty() != !index.truncated) {
    __builtin_trap();
  }
  uint64_t total_ms = 0;
  for (uint32_t ms : index.duration_ms) {
    if (ms == 0) __builtin_trap();
    total_ms += ms;
  }
  if (total_ms > limits.max_duration_ms) __builtin_trap();

  wt_source* source = nullptr;
  if (wt_source_create_memory(data, size, &limits, &source) != WT_OK) return 0;
  Decode(source, 24, 20, limits.max_frames);
  Decode(source, 0, 0, limits.max_frames);
  wt_source_destroy(source);
  return 0;
}
