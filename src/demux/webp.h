// Indexing a WebP file without decoding pixels: canvas size, frame count,
// per-frame durations and the alpha flag, all from libwebp's demuxer
// (WebPDemux; no VP8/VP8L bitstream is touched).
//
// Policies (ADR 0022):
//   - A still (non-animated) WebP is a one-frame animation.
//   - A frame duration of 10 ms or less, which includes the 0 that stills and
//     badly muxed files carry, plays for 100 ms, as browsers do. Everything
//     else is the file's value. Durations are therefore always >= 1, and
//     pts is the running sum.
//   - Limits are enforced like the WebM demuxer does: max_dimension on the
//     canvas, max_frames on the frame count, max_total_pixels on
//     frames x canvas area, and max_duration_ms on the summed durations.
//   - A file cut inside a frame keeps its complete frames: *out gets a
//     `prefix`, a copy of the bytes up to the last complete frame with the RIFF
//     size patched, which WebPAnimDecoder (it can't read partial files)
//     decodes. No complete frame is WT_ERR_TRUNCATED.
//
// Thread: background thread. No shared state.
#ifndef WT_DEMUX_WEBP_H_
#define WT_DEMUX_WEBP_H_

#include <cstdint>
#include <vector>

#include "api/wt_api.h"
#include "demux/source.h"

namespace wt {

// What a decoder is shown for a file that ends inside a frame (see above).
struct WebpIndex {
  uint32_t width = 0;   // Canvas size.
  uint32_t height = 0;  //
  bool has_alpha = false;
  bool truncated = false;
  // One entry per playable frame; each is >= 1 ms after the policy above.
  std::vector<uint32_t> duration_ms;
  // Non-empty only when `truncated`: the bytes to decode instead of the file.
  std::vector<uint8_t> prefix;
};

// Duration policy for one frame, in ms. Always >= 1.
uint32_t WebpFrameDurationMs(int duration);

// Indexes `bytes`. On failure *out is cleared. Errors: WT_ERR_INVALID_DATA,
// WT_ERR_TRUNCATED (no complete frame), WT_ERR_LIMIT,
// WT_ERR_INVALID_ARGUMENT (a zero limit), WT_ERR_NO_MEMORY.
wt_status DemuxWebp(ByteView bytes, const wt_limits& limits, WebpIndex* out);

}  // namespace wt

#endif  // WT_DEMUX_WEBP_H_
