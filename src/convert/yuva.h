// YUVA 4:2:0 frames, their scaling to a size bucket, and their conversion to
// premultiplied RGBA/BGRA.
//
// The decoders produce 8-bit YUV 4:2:0 at source size, plus an alpha plane
// when the video has one. wt_anim scales each frame into a YuvaImage at the
// output size (the size bucket), which is also the shape the frame cache
// (T3.1) will store, then converts it straight into the caller's buffer.
//
// Scaling happens before premultiplying, on straight alpha, as PLAN.md's
// pipeline has it (scale to the bucket -> YUVA420 -> premultiplied RGBA).
// Every routine here is libyuv, so ARM builds get its NEON paths.
//
// Thread: none of this has shared state. A YuvaImage is not thread-safe.
#ifndef WT_CONVERT_YUVA_H_
#define WT_CONVERT_YUVA_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "api/wt_api.h"

namespace wt {

// The YUV -> RGB matrix a stream signals (spike A, pitfall 7).
enum class YuvMatrix : uint8_t { kBt601, kBt709, kBt2020 };

struct YuvColor {
  YuvMatrix matrix = YuvMatrix::kBt601;
  bool full_range = false;  // false: studio range (Y 16..235).
};

// Chroma width or height for a 4:2:0 plane: rounded up for odd sizes.
inline uint32_t ChromaSize(uint32_t luma) { return luma / 2 + (luma & 1); }

// Non-owning 8-bit YUV 4:2:0 planes, plus an optional alpha plane at luma
// size. `a` is null when the frame is opaque. Strides are positive.
struct YuvaView {
  const uint8_t* y = nullptr;
  const uint8_t* u = nullptr;
  const uint8_t* v = nullptr;
  const uint8_t* a = nullptr;
  int y_stride = 0;
  int u_stride = 0;
  int v_stride = 0;
  int a_stride = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  YuvColor color;
};

// An owned YUVA 4:2:0 frame of a fixed size, in one allocation.
class YuvaImage {
 public:
  // Largest width or height Allocate accepts. wt_limits.max_dimension is
  // checked by callers; this bound only keeps the size arithmetic in range.
  static constexpr uint32_t kMaxDimension = 16384;

  YuvaImage() = default;
  YuvaImage(const YuvaImage&) = delete;
  YuvaImage& operator=(const YuvaImage&) = delete;

  // Allocates width x height (each 1..kMaxDimension), with an alpha plane
  // when with_alpha. Colour planes start black; the alpha plane starts at 255
  // (opaque). Returns WT_ERR_INVALID_ARGUMENT or WT_ERR_NO_MEMORY.
  wt_status Allocate(uint32_t width, uint32_t height, bool with_alpha);

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  bool has_alpha() const { return a_ != nullptr; }
  YuvColor color() const { return color_; }
  void set_color(YuvColor color) { color_ = color; }

  // The planes; a is null without an alpha plane.
  YuvaView view() const;

  // The single buffer behind the planes: Y (width x height), U and V
  // (ChromaSize(width) x ChromaSize(height) each) and, with alpha, A, packed
  // in that order with no padding. This is the frame cache's frame layout.
  uint8_t* data() { return buffer_.get(); }
  size_t data_size() const { return data_size_; }

 private:
  friend wt_status ScaleColor(const YuvaView& src, YuvaImage* dst);
  friend wt_status ScaleAlpha(const YuvaView& src, YuvaImage* dst);
  friend wt_status RgbaToYuva(const uint8_t* rgba, size_t stride,
                              uint32_t width, uint32_t height, YuvaImage* dst);

  std::unique_ptr<uint8_t[]> buffer_;
  uint8_t* y_ = nullptr;
  uint8_t* u_ = nullptr;
  uint8_t* v_ = nullptr;
  uint8_t* a_ = nullptr;  // Null without an alpha plane.
  size_t data_size_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  YuvColor color_;
};

// Scales src's Y, U and V planes to dst's size (box filter, which falls back
// to bilinear when enlarging) and takes over src's colour signalling. A same-
// size "scale" is a copy. dst keeps its alpha plane.
// Returns WT_ERR_INVALID_ARGUMENT for an empty src or dst.
wt_status ScaleColor(const YuvaView& src, YuvaImage* dst);

// Scales src's alpha plane into dst's alpha plane, like ScaleColor.
// Returns WT_ERR_INVALID_ARGUMENT when src.a is null or dst has no alpha.
wt_status ScaleAlpha(const YuvaView& src, YuvaImage* dst);

// Converts a straight-alpha (unpremultiplied) 8-bit RGBA canvas, bytes in
// R, G, B, A order, into dst, which must be exactly width x height. This is
// how the canvas decoders (WebP, GIF) join the YUVA path: they produce RGBA,
// and everything after this call is format-neutral.
//
// Colour is written as full-range BT.601 (libyuv's "J420") and dst's colour
// signalling is set to match, because 4:2:0 subsampling already loses chroma
// detail and the full range loses the least of the 8 bits. When dst has an
// alpha plane it receives the canvas alpha exactly; without one the canvas
// alpha is ignored (callers allocate dst without alpha only for opaque
// sources). The canvas must be readable for stride * (height - 1) + width * 4
// bytes. Returns WT_ERR_INVALID_ARGUMENT for a null canvas, a size that
// differs from dst's, or a stride below width * 4 (or above INT_MAX).
wt_status RgbaToYuva(const uint8_t* rgba, size_t stride, uint32_t width,
                     uint32_t height, YuvaImage* dst);

// Checks that a caller's buffer can hold width x height 4-byte pixels:
// stride >= width * 4, stride fits libyuv's int, and
// dst_size >= stride * (height - 1) + width * 4. WT_ERR_INVALID_ARGUMENT if
// not.
wt_status CheckPixelBuffer(uint32_t width, uint32_t height, size_t dst_size,
                           uint32_t stride);

// Converts src into dst as premultiplied 8-bit RGBA or BGRA (by memory
// order), using the matrix and range in src.color. Without an alpha plane
// every alpha byte is 255. Checks the buffer with CheckPixelBuffer first and
// writes only the src.width x src.height pixels, never row padding.
wt_status YuvaToPremultiplied(const YuvaView& src, uint8_t* dst,
                              size_t dst_size, uint32_t stride,
                              wt_pixel_format format);

}  // namespace wt

#endif  // WT_CONVERT_YUVA_H_
