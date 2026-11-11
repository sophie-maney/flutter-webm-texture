#include "decode/webp_decoder.h"

#include <new>

#include "webp/demux.h"

namespace wt {

wt_status WebpDecoder::Create(ByteView bytes, uint32_t width, uint32_t height,
                              uint32_t frame_count,
                              std::unique_ptr<WebpDecoder>* out) {
  out->reset();
  std::unique_ptr<WebpDecoder> d(new (std::nothrow) WebpDecoder);
  if (d == nullptr) return WT_ERR_NO_MEMORY;
  WebPAnimDecoderOptions options;
  if (!WebPAnimDecoderOptionsInit(&options)) return WT_ERR_INTERNAL;
  options.color_mode = MODE_RGBA;  // Straight alpha (MODE_rgbA premultiplies).
  options.use_threads = 0;
  WebPData data;
  WebPDataInit(&data);
  data.bytes = bytes.data;
  data.size = bytes.size;
  d->dec_ = WebPAnimDecoderNew(&data, &options);
  if (d->dec_ == nullptr) return WT_ERR_INVALID_DATA;
  WebPAnimInfo info;
  if (!WebPAnimDecoderGetInfo(d->dec_, &info)) return WT_ERR_INTERNAL;
  if (info.canvas_width != width || info.canvas_height != height ||
      info.frame_count != frame_count) {
    return WT_ERR_INVALID_DATA;
  }
  d->width_ = width;
  d->height_ = height;
  d->frame_count_ = frame_count;
  *out = std::move(d);
  return WT_OK;
}

WebpDecoder::~WebpDecoder() {
  if (dec_ != nullptr) WebPAnimDecoderDelete(dec_);
}

wt_status WebpDecoder::Reset() {
  WebPAnimDecoderReset(dec_);
  position_ = 0;
  return WT_OK;
}

wt_status WebpDecoder::Next(RgbaView* out) {
  *out = RgbaView();
  if (position_ >= frame_count_ || !WebPAnimDecoderHasMoreFrames(dec_)) {
    return WT_END;
  }
  uint8_t* canvas = nullptr;
  int timestamp = 0;
  if (!WebPAnimDecoderGetNext(dec_, &canvas, &timestamp) || canvas == nullptr) {
    return WT_ERR_DECODE;
  }
  ++position_;
  out->data = canvas;
  out->stride = size_t{width_} * 4;
  out->width = width_;
  out->height = height_;
  return WT_OK;
}

}  // namespace wt
