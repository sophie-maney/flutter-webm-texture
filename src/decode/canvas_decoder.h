// The seam between wt::Anim and the formats that decode to a full RGBA canvas
// one frame after another: animated WebP (WebpDecoder) and GIF (GifDecoder).
// WebM keeps its own path (VpxDecoder), which yields YUVA directly.
//
// A canvas decoder is sequential: Next() produces frame position(), already
// composited with the frames before it (disposal and blending are the
// decoder's job). Going backwards means Reset() and decoding forward again.
// Anim turns each canvas into the YUVA cache path with RgbaToYuva
// (convert/yuva.h), which is the same for every canvas format.
//
// Thread: background thread. Not thread-safe; one decoder per animation.
#ifndef WT_DECODE_CANVAS_DECODER_H_
#define WT_DECODE_CANVAS_DECODER_H_

#include <cstddef>
#include <cstdint>

#include "api/wt_api.h"

namespace wt {

// A straight-alpha RGBA canvas (bytes R, G, B, A), owned by the decoder and
// valid until its next Next, Reset or destruction.
struct RgbaView {
  const uint8_t* data = nullptr;
  size_t stride = 0;  // Bytes; >= width * 4.
  uint32_t width = 0;
  uint32_t height = 0;
};

class CanvasDecoder {
 public:
  virtual ~CanvasDecoder() = default;

  // The index of the frame the next Next() produces.
  virtual uint32_t position() const = 0;

  // Back to frame 0. WT_OK or WT_ERR_INTERNAL.
  virtual wt_status Reset() = 0;

  // Decodes frame position() into *out and advances. Errors (WT_ERR_DECODE,
  // WT_ERR_NO_MEMORY, WT_END past the last frame) leave position() unchanged;
  // after a decode error the caller must Reset() before trusting the decoder
  // again, since compositing state may be damaged.
  virtual wt_status Next(RgbaView* out) = 0;
};

}  // namespace wt

#endif  // WT_DECODE_CANVAS_DECODER_H_
