#include "convert/yuva.h"

#include <climits>
#include <cstring>
#include <new>

#include "libyuv/convert_argb.h"
#include "libyuv/convert_from_argb.h"
#include "libyuv/planar_functions.h"
#include "libyuv/scale.h"

namespace wt {

namespace {

// libyuv's matrices for a colour signalling. The Yvu twins are for RGBA
// output: libyuv writes RGBA ("ABGR") by swapping U and V and using the
// mirrored matrix, which is what its own I420AlphaToABGR does.
const libyuv::YuvConstants* Matrix(YuvColor color, bool swap_uv) {
  switch (color.matrix) {
    case YuvMatrix::kBt709:
      if (color.full_range) {
        return swap_uv ? &libyuv::kYvuF709Constants
                       : &libyuv::kYuvF709Constants;
      }
      return swap_uv ? &libyuv::kYvuH709Constants : &libyuv::kYuvH709Constants;
    case YuvMatrix::kBt2020:
      if (color.full_range) {
        return swap_uv ? &libyuv::kYvuV2020Constants
                       : &libyuv::kYuvV2020Constants;
      }
      return swap_uv ? &libyuv::kYvu2020Constants : &libyuv::kYuv2020Constants;
    case YuvMatrix::kBt601:
      break;
  }
  if (color.full_range) {
    return swap_uv ? &libyuv::kYvuJPEGConstants : &libyuv::kYuvJPEGConstants;
  }
  return swap_uv ? &libyuv::kYvuI601Constants : &libyuv::kYuvI601Constants;
}

bool ViewValid(const YuvaView& v) {
  return v.y != nullptr && v.u != nullptr && v.v != nullptr && v.width > 0 &&
         v.height > 0 && v.width <= YuvaImage::kMaxDimension &&
         v.height <= YuvaImage::kMaxDimension &&
         v.y_stride >= static_cast<int>(v.width) &&
         v.u_stride >= static_cast<int>(ChromaSize(v.width)) &&
         v.v_stride >= static_cast<int>(ChromaSize(v.width)) &&
         (v.a == nullptr || v.a_stride >= static_cast<int>(v.width));
}

int I(uint32_t value) { return static_cast<int>(value); }

}  // namespace

wt_status YuvaImage::Allocate(uint32_t width, uint32_t height,
                              bool with_alpha) {
  if (width == 0 || height == 0 || width > kMaxDimension ||
      height > kMaxDimension) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  // At most 16384^2 * 2.5 bytes: no overflow in size_t on 64-bit, and on
  // 32-bit the dimension limits callers apply keep it far below 4 GiB.
  const size_t luma = size_t{width} * height;
  const size_t chroma = size_t{ChromaSize(width)} * ChromaSize(height);
  const size_t total = luma + 2 * chroma + (with_alpha ? luma : 0);
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[total]);
  if (buffer == nullptr) return WT_ERR_NO_MEMORY;

  buffer_ = std::move(buffer);
  data_size_ = total;
  width_ = width;
  height_ = height;
  color_ = YuvColor{};
  y_ = buffer_.get();
  u_ = y_ + luma;
  v_ = u_ + chroma;
  a_ = with_alpha ? v_ + chroma : nullptr;
  // Studio-range black, and opaque alpha until a frame provides one.
  std::memset(y_, 16, luma);
  std::memset(u_, 128, 2 * chroma);
  if (a_ != nullptr) std::memset(a_, 255, luma);
  return WT_OK;
}

YuvaView YuvaImage::view() const {
  YuvaView v;
  v.y = y_;
  v.u = u_;
  v.v = v_;
  v.a = a_;
  v.y_stride = I(width_);
  v.u_stride = I(ChromaSize(width_));
  v.v_stride = I(ChromaSize(width_));
  v.a_stride = a_ != nullptr ? I(width_) : 0;
  v.width = width_;
  v.height = height_;
  v.color = color_;
  return v;
}

wt_status ScaleColor(const YuvaView& src, YuvaImage* dst) {
  if (dst == nullptr || dst->buffer_ == nullptr || !ViewValid(src)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  const YuvaView d = dst->view();
  if (libyuv::I420Scale(src.y, src.y_stride, src.u, src.u_stride, src.v,
                        src.v_stride, I(src.width), I(src.height), dst->y_,
                        d.y_stride, dst->u_, d.u_stride, dst->v_, d.v_stride,
                        I(d.width), I(d.height), libyuv::kFilterBox) != 0) {
    return WT_ERR_INTERNAL;
  }
  dst->color_ = src.color;
  return WT_OK;
}

wt_status ScaleAlpha(const YuvaView& src, YuvaImage* dst) {
  if (dst == nullptr || dst->a_ == nullptr || src.a == nullptr ||
      !ViewValid(src)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  if (libyuv::ScalePlane(src.a, src.a_stride, I(src.width), I(src.height),
                         dst->a_, I(dst->width_), I(dst->width_),
                         I(dst->height_), libyuv::kFilterBox) != 0) {
    return WT_ERR_INTERNAL;
  }
  return WT_OK;
}

wt_status RgbaToYuva(const uint8_t* rgba, size_t stride, uint32_t width,
                     uint32_t height, YuvaImage* dst) {
  if (rgba == nullptr || dst == nullptr || dst->buffer_ == nullptr ||
      width != dst->width_ || height != dst->height_ ||
      stride < size_t{width} * 4 || stride > static_cast<size_t>(INT_MAX)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  const YuvaView d = dst->view();
  // libyuv "ABGR" is R, G, B, A in memory.
  if (libyuv::ABGRToJ420(rgba, static_cast<int>(stride), dst->y_, d.y_stride,
                         dst->u_, d.u_stride, dst->v_, d.v_stride, I(width),
                         I(height)) != 0) {
    return WT_ERR_INTERNAL;
  }
  if (dst->a_ != nullptr &&
      libyuv::ARGBExtractAlpha(rgba, static_cast<int>(stride), dst->a_,
                               d.a_stride, I(width), I(height)) != 0) {
    return WT_ERR_INTERNAL;
  }
  dst->color_ = YuvColor{YuvMatrix::kBt601, /*full_range=*/true};
  return WT_OK;
}

wt_status CheckPixelBuffer(uint32_t width, uint32_t height, size_t dst_size,
                           uint32_t stride) {
  if (width == 0 || height == 0) return WT_ERR_INVALID_ARGUMENT;
  const uint64_t row = uint64_t{width} * 4;
  if (stride < row || stride > static_cast<uint32_t>(INT_MAX)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  // < 2^32 * 2^32 + 2^34: fits in uint64.
  const uint64_t needed = uint64_t{stride} * (height - 1) + row;
  if (dst_size < needed) return WT_ERR_INVALID_ARGUMENT;
  return WT_OK;
}

wt_status YuvaToPremultiplied(const YuvaView& src, uint8_t* dst,
                              size_t dst_size, uint32_t stride,
                              wt_pixel_format format) {
  if (dst == nullptr || !ViewValid(src)) return WT_ERR_INVALID_ARGUMENT;
  if (format != WT_PIXEL_FORMAT_RGBA8888 &&
      format != WT_PIXEL_FORMAT_BGRA8888) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  const wt_status s = CheckPixelBuffer(src.width, src.height, dst_size, stride);
  if (s != WT_OK) return s;

  // libyuv "ARGB" is B, G, R, A in memory (our BGRA8888). RGBA8888 is its
  // "ABGR": the same call with U and V swapped and the mirrored matrix.
  const bool rgba = format == WT_PIXEL_FORMAT_RGBA8888;
  const uint8_t* u = rgba ? src.v : src.u;
  const uint8_t* v = rgba ? src.u : src.v;
  const int u_stride = rgba ? src.v_stride : src.u_stride;
  const int v_stride = rgba ? src.u_stride : src.v_stride;
  const libyuv::YuvConstants* matrix = Matrix(src.color, rgba);
  int r;
  if (src.a != nullptr) {
    // attenuate = 1 premultiplies in the same pass: (c * a + 255) >> 8,
    // within 1 of exact (spike A, pitfall 10).
    r = libyuv::I420AlphaToARGBMatrix(
        src.y, src.y_stride, u, u_stride, v, v_stride, src.a, src.a_stride, dst,
        static_cast<int>(stride), matrix, I(src.width), I(src.height),
        /*attenuate=*/1);
  } else {
    // Writes alpha = 255.
    r = libyuv::I420ToARGBMatrix(src.y, src.y_stride, u, u_stride, v, v_stride,
                                 dst, static_cast<int>(stride), matrix,
                                 I(src.width), I(src.height));
  }
  return r == 0 ? WT_OK : WT_ERR_INTERNAL;
}

}  // namespace wt
