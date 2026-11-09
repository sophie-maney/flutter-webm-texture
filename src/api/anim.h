// wt::Anim: the playback cursor behind a wt_anim handle.
//
// T1.4 scope: WebM (VP8/VP9, with or without alpha) only, decoded
// synchronously on every NextFrame with no frame cache. WebP and GIF arrive
// in Phase 5, the cache in T3.1, and limit enforcement beyond what the
// demuxer checks (duration, pixel budget) in T1.5.
//
// Per frame: decode at source size (VpxDecoder) -> scale into a YUVA 4:2:0
// frame at the output size (YuvaImage) -> premultiplied RGBA/BGRA in the
// caller's buffer. The scaled frame persists between calls, which is how a
// packet that shows nothing repeats the previous frame and how a frame with
// a broken alpha payload keeps the last good alpha.
//
// Thread: background thread. Not thread-safe (see wt_api.h, Threading).
#ifndef WT_API_ANIM_H_
#define WT_API_ANIM_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "api/wt_api.h"
#include "convert/yuva.h"
#include "decode/vpx_decoder.h"
#include "demux/source.h"
#include "demux/webm.h"

namespace wt {

// Fills *out from a demuxed WebM whose status is WT_OK or (with frames)
// WT_ERR_TRUNCATED. Durations saturate rather than overflow.
void WebmInfo(const WebmIndex& index, wt_info* out);

class Anim {
 public:
  // Demuxes `bytes` (which must outlive the Anim) and prepares to decode it
  // at width x height, or at source size for 0 x 0. A non-zero size must be
  // in 1..limits.max_dimension on both axes (WT_ERR_INVALID_ARGUMENT).
  // Fails like the demuxer for unplayable input; WebP and GIF are
  // WT_ERR_UNSUPPORTED until Phase 5.
  static wt_status Create(ByteView bytes, wt_format format,
                          const wt_limits& limits, uint32_t width,
                          uint32_t height, std::unique_ptr<Anim>* out);

  ~Anim();
  Anim(const Anim&) = delete;
  Anim& operator=(const Anim&) = delete;

  const wt_info& info() const { return info_; }
  uint32_t width() const { return frame_.width(); }
  uint32_t height() const { return frame_.height(); }

  // See wt_anim_seek. Cheap: the decoding happens in the next NextFrame.
  wt_status Seek(uint32_t frame_index);

  // See wt_anim_next_frame. The cursor advances on errors too, so a broken
  // frame doesn't stall playback. An invalid buffer is rejected before any
  // decoding and doesn't advance it.
  wt_status NextFrame(uint8_t* dst, size_t dst_size, uint32_t stride,
                      wt_pixel_format format, wt_frame_info* out_frame);

 private:
  Anim() = default;

  // The last random-access frame at or before index, or 0 when none is.
  uint32_t RandomAccessBefore(uint32_t index) const;
  // Feeds frame `index` to the decoder; when present, also scales its output
  // into frame_.
  wt_status DecodeFrame(uint32_t index, bool present);

  ByteView bytes_;
  WebmIndex index_;
  wt_info info_{};
  VpxDecoder decoder_;
  YuvaImage frame_;          // The current frame at the output size.
  bool have_frame_ = false;  // frame_ holds a decoded frame.
  // The last packet decoded, so the decoder can continue from next_decode_
  // rather than waiting for a keyframe.
  bool decoder_healthy_ = true;
  uint32_t cursor_ = 0;       // Next frame NextFrame returns.
  uint32_t next_decode_ = 0;  // Next frame the decoder expects.
};

}  // namespace wt

#endif  // WT_API_ANIM_H_
