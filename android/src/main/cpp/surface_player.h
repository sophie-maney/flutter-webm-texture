// SurfacePlayer: one animation drawn into one ANativeWindow. The core's
// scheduler (wt_sched, T3.3) decides when frames are drawn and runs them on
// its workers; this class is the wt_sink that hands it locked window buffers.
// T2.2 gave each animation its own render thread instead.
//
// Threads:
// - Create runs on a background thread (the caller made the anim there).
// - SetWindow, SetActive, SetPriority and SeekTo run on the platform main
//   thread. None of them decodes. SetWindow waits for at most one frame being
//   written into the old window (lock, YUV to RGBA, post), so the window is
//   never used after it returns; the core decodes before it acquires the
//   window (T6.1, S-05, ADR 0027), so that wait never includes a decode.
// - The destructor runs on a background thread (nativeDestroy), after
//   SetWindow(nullptr): it cancels the frame in flight and waits for it to
//   stop.
// - The sink callbacks run on scheduler workers, one at a time.
#ifndef WEBM_TEXTURE_ANDROID_SURFACE_PLAYER_H_
#define WEBM_TEXTURE_ANDROID_SURFACE_PLAYER_H_

#include <android/native_window.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "wt_api.h"

namespace wt_android {

// Called on a scheduler worker for a failure after creation. code is a
// wt_status name or "SURFACE_LOST".
using ErrorCallback =
    std::function<void(const std::string& code, const std::string& message)>;

class SurfacePlayer {
 public:
  // Puts anim (width x height output, as created) on sched. On success the
  // player owns anim; on failure the caller keeps it and *out_status says
  // why. With loop false, playback stops on the last frame and holds it
  // until SeekTo. on_error must not call back into the player.
  static std::unique_ptr<SurfacePlayer> Create(wt_sched* sched, wt_anim* anim,
                                               uint32_t width, uint32_t height,
                                               bool loop,
                                               ErrorCallback on_error,
                                               wt_status* out_status);

  // Takes the player off the scheduler (cancelling the frame in flight and
  // waiting for it to stop), destroys the anim and releases the window.
  // Background thread.
  ~SurfacePlayer();

  SurfacePlayer(const SurfacePlayer&) = delete;
  SurfacePlayer& operator=(const SurfacePlayer&) = delete;

  // Replaces the window (taking ownership of the caller's reference) and
  // redraws into it. nullptr releases the window: when this returns, the
  // player no longer uses it, and the scheduler stops drawing until the next
  // window. Returns the status of ANativeWindow_setBuffersGeometry as a
  // wt_status (WT_OK or WT_ERR_INTERNAL); on failure the window is released.
  wt_status SetWindow(ANativeWindow* window);

  // Starts or stops playback. Stopped players keep showing their frame.
  void SetActive(bool active);

  // The on-screen area of the widgets showing this animation, in physical
  // pixels: the scheduler's rank under its max_concurrent limit.
  void SetPriority(uint64_t priority);

  // Shows frame (0-based) as soon as a worker is free, whether or not the
  // player is active; an active player carries on from there. Without a
  // window, the frame is drawn when one arrives. Returns
  // WT_ERR_INVALID_ARGUMENT when frame is past the last frame.
  wt_status SeekTo(uint32_t frame);

  // The scheduler's handle, for wt_atlas_attach and wt_atlas_detach. While
  // the player is in an atlas, its frames go there and this object only
  // reports errors; without a window it parks (WT_NO_TARGET) when detached.
  wt_player* player() const { return player_; }

 private:
  SurfacePlayer(uint32_t width, uint32_t height, ErrorCallback on_error);

  // wt_sink. Acquire locks window_mu_ and, on WT_OK, leaves it locked for
  // Release (same worker thread), so a lock-write-post is atomic with
  // respect to SetWindow. The frame is already decoded when Acquire runs.
  static wt_status Acquire(void* context, wt_buffer* out);
  static void Release(void* context, wt_status status,
                      const wt_frame_info* frame);
  static void Error(void* context, wt_status status);

  void ReportSurfaceOnce(const std::string& message);

  const uint32_t width_;
  const uint32_t height_;
  const ErrorCallback on_error_;
  wt_player* player_ = nullptr;

  // Guards window_. Held by a worker for the length of one lock-write-post
  // (no decode), so SetWindow(nullptr) can't return mid-write.
  std::mutex window_mu_;
  ANativeWindow* window_ = nullptr;
  // Worker only (callbacks are serialised): a surface failure was reported
  // and no frame has been drawn since.
  bool reported_surface_ = false;
};

}  // namespace wt_android

#endif  // WEBM_TEXTURE_ANDROID_SURFACE_PLAYER_H_
