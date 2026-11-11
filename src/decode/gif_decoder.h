// GIF frames behind the CanvasDecoder seam: giflib's low-level reader (never
// DGifSlurp) plus our own compositing onto a straight-alpha RGBA canvas at
// the logical-screen size. One frame per Next(), sequential; Reset() reopens
// the stream and clears the canvas.
//
// Compositing (ADR 0001, ADR 0023):
//   - The canvas starts fully transparent (0, 0, 0, 0), as it does again
//     after Reset(), which is what a loop restart shows.
//   - Disposal applies after a frame has been shown, i.e. at the start of
//     the next Next(). "Restore to background" clears the frame's rect to
//     transparent, never to the palette's background colour. "Restore to
//     previous" puts back the rect as it was before the frame was drawn
//     (transparent for the first frame). "None" and "leave" keep the frame.
//   - The frame's transparency index leaves the canvas pixel below unchanged.
//     A local colour map overrides the global one. A pixel index the palette
//     doesn't have, or any pixel of a frame with no colour map at all, is
//     treated like the transparency index: it draws nothing.
//   - Interlaced frames are de-interlaced. Rows and columns outside the
//     canvas are decoded (the LZW stream has to be consumed) and dropped.
//
// The decoder is driven by a GifIndex from DemuxGif, which holds each
// frame's rect, disposal and transparency; the stream is checked against it
// and a disagreement is WT_ERR_DECODE. After any error the decoder refuses
// further frames until Reset() (the canvas is part-drawn).
//
// Thread: background thread. Not thread-safe; one decoder per animation.
#ifndef WT_DECODE_GIF_DECODER_H_
#define WT_DECODE_GIF_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "decode/canvas_decoder.h"
#include "demux/gif.h"
#include "demux/source.h"

namespace wt {

class GifDecoder final : public CanvasDecoder {
 public:
  // `bytes` must be the whole file DemuxGif indexed and outlive the decoder.
  // Errors: WT_ERR_INVALID_DATA (the stream disagrees with the index),
  // WT_ERR_NO_MEMORY, WT_ERR_INVALID_ARGUMENT (an empty or oversized index).
  static wt_status Create(ByteView bytes, const GifIndex& index,
                          std::unique_ptr<GifDecoder>* out);
  ~GifDecoder() override;

  uint32_t position() const override { return position_; }
  wt_status Reset() override;
  wt_status Next(RgbaView* out) override;

 private:
  GifDecoder() = default;

  // A frame's rect clipped to the canvas, [x0, x1) x [y0, y1).
  struct Clip {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    size_t bytes() const { return size_t{x1 - x0} * (y1 - y0) * 4; }
  };
  Clip ClipOf(const GifFrame& f) const;
  void ApplyDisposal();
  wt_status ReachImage(const GifFrame& f);
  wt_status DrawImage(const GifFrame& f, const Clip& clip);

  ByteView bytes_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<GifFrame> frames_;
  std::unique_ptr<GifStream> stream_;
  std::unique_ptr<uint8_t[]> canvas_;  // width_ x height_ RGBA.
  std::unique_ptr<uint8_t[]> saved_;   // For "restore to previous".
  std::unique_ptr<uint8_t[]> line_;    // One decoded row of indices.
  size_t saved_capacity_ = 0;
  size_t line_capacity_ = 0;
  uint32_t position_ = 0;
  bool failed_ = false;
  // The disposal still owed for the last frame drawn.
  uint8_t pending_disposal_ = 0;
  Clip pending_clip_;
};

}  // namespace wt

#endif  // WT_DECODE_GIF_DECODER_H_
