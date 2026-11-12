// WebPAnimDecoder behind the CanvasDecoder seam: frames of an animated (or
// still) WebP composited onto a straight-alpha RGBA canvas at source size,
// honouring blend and disposal methods. Single-threaded (use_threads = 0).
//
// The caller has already indexed the file (DemuxWebp) and so knows its size
// and frame count; Create re-checks the canvas against them, which makes a
// disagreement between the two parses an error rather than a bounds problem.
#ifndef WT_DECODE_WEBP_DECODER_H_
#define WT_DECODE_WEBP_DECODER_H_

#include <cstdint>
#include <memory>

#include "decode/canvas_decoder.h"
#include "demux/source.h"

struct WebPAnimDecoder;

namespace wt {

class WebpDecoder final : public CanvasDecoder {
 public:
  // `bytes` must be a whole file (DemuxWebp's `prefix` when it made one) and
  // outlive the decoder. width, height and frame_count are what DemuxWebp
  // reported. Errors: WT_ERR_INVALID_DATA, WT_ERR_NO_MEMORY.
  static wt_status Create(ByteView bytes, uint32_t width, uint32_t height,
                          uint32_t frame_count,
                          std::unique_ptr<WebpDecoder>* out);
  ~WebpDecoder() override;

  uint32_t position() const override { return position_; }
  wt_status Reset() override;
  wt_status Next(RgbaView* out) override;

 private:
  WebpDecoder() = default;

  WebPAnimDecoder* dec_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t frame_count_ = 0;
  uint32_t position_ = 0;
};

}  // namespace wt

#endif  // WT_DECODE_WEBP_DECODER_H_
