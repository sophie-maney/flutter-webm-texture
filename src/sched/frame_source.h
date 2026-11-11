// wt::FrameSource: what the scheduler plays. The C ABI wraps a wt_anim in
// one (api/sched_api.cc); tests use fakes.
#ifndef WT_SCHED_FRAME_SOURCE_H_
#define WT_SCHED_FRAME_SOURCE_H_

#include <cstdint>

#include "api/wt_api.h"

namespace wt {

// A playback cursor over frames, like wt_anim. The scheduler calls at most
// one method at a time, from any of its workers. frame_count,
// frame_duration_ms and GetOutputSize must not change, and may be called from
// any thread at any time (the atlas reads the size while a frame decodes).
class FrameSource {
 public:
  virtual ~FrameSource() = default;

  // The size, in pixels, every frame is drawn at; 0 x 0 when unknown. An
  // atlas (src/atlas/compositor.h) sizes the player's slot from it.
  virtual void GetOutputSize(uint32_t* width, uint32_t* height) const {
    *width = 0;
    *height = 0;
  }

  // At least 1.
  virtual uint32_t frame_count() const = 0;
  // How long frame `index` (< frame_count) is shown, in ms.
  virtual uint32_t frame_duration_ms(uint32_t index) const = 0;
  // As wt_anim_seek.
  virtual wt_status Seek(uint32_t index) = 0;
  // Moves the cursor past one frame without drawing it (wt::Anim::SkipFrame).
  virtual wt_status Skip() = 0;
  // As wt_anim_next_frame, into dst.
  virtual wt_status Draw(const wt_buffer& dst, wt_frame_info* out_frame) = 0;
};

}  // namespace wt

#endif  // WT_SCHED_FRAME_SOURCE_H_
