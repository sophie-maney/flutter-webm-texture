// wt::FrameSource: what the scheduler plays. The C ABI wraps a wt_anim in
// one (api/sched_api.cc); tests use fakes.
#ifndef WT_SCHED_FRAME_SOURCE_H_
#define WT_SCHED_FRAME_SOURCE_H_

#include <cstdint>

#include "api/cancel.h"
#include "api/wt_api.h"

namespace wt {

// A playback cursor over frames, like wt_anim. The scheduler calls at most
// one method at a time, from any of its workers. frame_count,
// frame_duration_ms and GetOutputSize must not change, and may be called from
// any thread at any time (the atlas reads the size while a frame decodes).
//
// Drawing a frame takes two calls (T6.1, S-05, ADR 0027): Prepare decodes it
// before the scheduler takes a buffer from the sink, which may take long (a
// catch-up from the last keyframe); Write then only converts it into the
// buffer, so the sink's buffer, and on Android the window lock a platform
// thread may wait on, is held for the conversion alone.
//
// Cancellation: Skip and Prepare get the player's CancelFlag, which another
// thread sets to have the job end early. A source whose call can take long
// checks it between frames and returns kCancelled, its cursor where it was
// before the call; the next call carries on.
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
  // As wt_anim_seek: cheap, the decoding happens in the next Skip or Prepare.
  virtual wt_status Seek(uint32_t index) = 0;
  // Moves the cursor past one frame without drawing it (wt::Anim::SkipFrame).
  virtual wt_status Skip(const CancelFlag& cancel) = 0;
  // Decodes the frame at the cursor and keeps it for Write, advancing the
  // cursor (on errors too; WT_END at the end). *out_frame describes it.
  virtual wt_status Prepare(const CancelFlag& cancel,
                            wt_frame_info* out_frame) = 0;
  // Writes the frame the last successful Prepare kept into dst. The frame
  // stays kept until the next Seek, Skip or Prepare. WT_ERR_INVALID_ARGUMENT
  // for a buffer that doesn't fit.
  virtual wt_status Write(const wt_buffer& dst) = 0;
};

}  // namespace wt

#endif  // WT_SCHED_FRAME_SOURCE_H_
