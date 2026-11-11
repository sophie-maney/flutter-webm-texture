// AtlasWindow: one atlas page's ANativeWindow, handed to the core as a
// wt_atlas_target (T4.2, ADR 0020). The core's atlas (wt_atlas) writes every
// live slot of the page into one locked buffer per tick and posts it; the
// SurfaceProducer's ImageReader then schedules the engine frame itself (ADR
// 0011), so there is one post, and one frame request, per page per tick.
//
// Threads:
// - Create and the destructor run on the platform main thread. The owner
//   destroys a window only after wt_atlas_set_target replaced it (or
//   wt_atlas_destroy returned), so the callbacks never see a dead one.
// - The target callbacks run on scheduler workers, one at a time.
#ifndef WEBM_TEXTURE_ANDROID_ATLAS_WINDOW_H_
#define WEBM_TEXTURE_ANDROID_ATLAS_WINDOW_H_

#include <android/native_window.h>

#include <cstdint>
#include <memory>

#include "wt_api.h"

namespace wt_android {

class AtlasWindow {
 public:
  // Takes ownership of window and sets its geometry to width x height
  // RGBA_8888. Returns nullptr (having released window) on failure.
  static std::unique_ptr<AtlasWindow> Create(ANativeWindow* window,
                                             uint32_t width, uint32_t height);
  ~AtlasWindow();

  AtlasWindow(const AtlasWindow&) = delete;
  AtlasWindow& operator=(const AtlasWindow&) = delete;

  wt_atlas_target target();

 private:
  AtlasWindow(ANativeWindow* window, uint32_t width, uint32_t height)
      : window_(window), width_(width), height_(height) {}

  // Locks the whole buffer (dirty rect NULL, so nothing is copied back from
  // the previous one); the buffer id is its address.
  static wt_status Acquire(void* context, wt_buffer* out, uint64_t* id);
  // Posts the buffer: a locked buffer can't be cancelled.
  static void Present(void* context, wt_status status);

  ANativeWindow* const window_;
  const uint32_t width_;
  const uint32_t height_;
  // Worker only (callbacks are serialised): a lock failure was logged and no
  // frame has been posted since.
  bool reported_ = false;
};

}  // namespace wt_android

#endif  // WEBM_TEXTURE_ANDROID_ATLAS_WINDOW_H_
