// VP8/VP9 decoding with alpha: one libvpx context for the colour stream and,
// for AlphaMode = 1 tracks, a second one for the alpha stream (the
// BlockAdditional payloads), created on the first alpha payload so opaque
// files pay nothing (spike A, pitfall 3).
//
// Output is YUVA 4:2:0 at source size, as views into libvpx's own images.
//
// Rules for hostile or damaged streams (spike A, pitfalls 2, 5, 6, 8):
//   - Both images must be 8-bit I420 at exactly the track's PixelWidth x
//     PixelHeight. A size change is WT_ERR_INVALID_DATA; another pixel format
//     (VP9 profiles 1-3) is WT_ERR_UNSUPPORTED.
//   - Each stream decodes only from a bitstream keyframe. A fresh decoder,
//     and one whose last packet failed, skips packets until the next
//     keyframe instead of feeding inter frames to a context that lacks their
//     references (VP8 would otherwise decode wrong pixels without an error).
//   - A colour packet that fails, or that skips while waiting for a
//     keyframe, is WT_ERR_DECODE.
//   - Alpha problems never fail a frame. When an AlphaMode = 1 frame has no
//     alpha payload, or its alpha fails to decode, the frame comes back with
//     no new alpha (alpha_fresh = false) and the caller keeps showing the
//     last good alpha plane. The alpha stream resumes at its next keyframe.
//   - A packet that shows no frame (hidden frames outside a superframe) is
//     not an error: shown = false and the caller repeats its last frame.
//
// Thread: background thread. Not thread-safe; one decoder per animation.
#ifndef WT_DECODE_VPX_DECODER_H_
#define WT_DECODE_VPX_DECODER_H_

#include <cstdint>
#include <memory>

#include "api/wt_api.h"
#include "convert/yuva.h"
#include "demux/source.h"

namespace wt {

class VpxDecoder {
 public:
  struct Output {
    // The colour packet produced a shown frame. When false, `view` is empty.
    bool shown = false;
    // view.a holds a newly decoded alpha plane. When false, view.a is null.
    bool alpha_fresh = false;
    // Valid until the next Decode or Reset, or the decoder's destruction.
    YuvaView view;
  };

  VpxDecoder();
  ~VpxDecoder();
  VpxDecoder(const VpxDecoder&) = delete;
  VpxDecoder& operator=(const VpxDecoder&) = delete;

  // Creates the colour context. codec is WT_CODEC_VP8 or WT_CODEC_VP9; width
  // and height are the track's (1..YuvaImage::kMaxDimension); track_alpha is
  // AlphaMode == 1. Returns WT_ERR_INVALID_ARGUMENT, WT_ERR_NO_MEMORY or
  // WT_ERR_INTERNAL. Call once.
  wt_status Init(wt_codec codec, uint32_t width, uint32_t height,
                 bool track_alpha);

  // Decodes one container frame: its colour payload and its alpha payload
  // (empty when the frame has none). Statuses: WT_OK, WT_ERR_DECODE,
  // WT_ERR_INVALID_DATA, WT_ERR_UNSUPPORTED, WT_ERR_NO_MEMORY. On an error
  // *out is reset (shown = false). Payload bytes are only read during the
  // call.
  wt_status Decode(ByteView color, ByteView alpha, Output* out);

 private:
  class Context;

  // Decodes the alpha payload, if the track has alpha. *plane stays null
  // when the frame gets no new alpha. Fails only for WT_ERR_NO_MEMORY.
  wt_status DecodeAlpha(ByteView alpha, const uint8_t** plane, int* stride);

  wt_codec codec_ = WT_CODEC_UNKNOWN;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool track_alpha_ = false;
  std::unique_ptr<Context> color_;
  std::unique_ptr<Context> alpha_;  // Created on the first alpha payload.
};

}  // namespace wt

#endif  // WT_DECODE_VPX_DECODER_H_
