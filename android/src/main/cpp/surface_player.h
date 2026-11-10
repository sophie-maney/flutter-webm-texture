// SurfacePlayer: one animation drawn into one ANativeWindow by its own render
// thread (T2.2). The T3.3 scheduler replaces the per-animation thread.
//
// Threads:
// - Create runs on a background thread (it probes and validates the source).
// - SetWindow, SetActive, SeekTo and the destructor run on the platform main
//   thread. None of them decodes; SetWindow and the destructor wait for at most
//   one in-flight frame write so the window is never used after they return.
// - Everything that touches the wt_anim runs on the render thread.
#ifndef WEBM_TEXTURE_ANDROID_SURFACE_PLAYER_H_
#define WEBM_TEXTURE_ANDROID_SURFACE_PLAYER_H_

#include <android/native_window.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "wt_api.h"

namespace wt_android {

// Called on the render thread for a failure after creation. code is a
// wt_status name or "SURFACE_LOST".
using ErrorCallback =
    std::function<void(const std::string& code, const std::string& message)>;

// Run on the render thread when it starts and just before it exits. The JNI
// bridge attaches the thread to the JVM here: otherwise the ImageReader
// listener attaches and detaches it around every posted frame, allocating a
// java.lang.Thread each time.
struct ThreadHooks {
  std::function<void()> on_start;
  std::function<void()> on_exit;
};

class SurfacePlayer {
 public:
  // Takes ownership of anim. width x height is the anim's output size. With
  // loop false, playback stops on the last frame and holds it until SeekTo.
  // on_error is called on the render thread and must not call back into the
  // player.
  static std::unique_ptr<SurfacePlayer> Create(wt_anim* anim, uint32_t width,
                                               uint32_t height,
                                               uint32_t frame_count, bool loop,
                                               ErrorCallback on_error,
                                               ThreadHooks hooks = {});

  // Stops the render thread, releases the window and destroys the anim.
  ~SurfacePlayer();

  SurfacePlayer(const SurfacePlayer&) = delete;
  SurfacePlayer& operator=(const SurfacePlayer&) = delete;

  // Replaces the window (taking ownership of the caller's reference) and
  // redraws the current frame into it. nullptr releases the window: when
  // this returns, the player no longer uses it. Returns the status of
  // ANativeWindow_setBuffersGeometry as a wt_status (WT_OK or
  // WT_ERR_INTERNAL); on failure the window is released.
  wt_status SetWindow(ANativeWindow* window);

  // Starts or stops playback. Stopped players keep showing their frame.
  void SetActive(bool active);

  // Shows frame (0-based) as soon as the render thread gets to it, whether
  // or not the player is active; an active player carries on from there.
  // Without a window, the frame is drawn when one arrives. Returns
  // WT_ERR_INVALID_ARGUMENT when frame >= frame_count.
  wt_status SeekTo(uint32_t frame);

 private:
  using Clock = std::chrono::steady_clock;

  SurfacePlayer(wt_anim* anim, uint32_t width, uint32_t height,
                uint32_t frame_count, bool loop, ErrorCallback on_error,
                ThreadHooks hooks);

  void Run();
  void Loop();
  // Moves the core's cursor (and cursor_) to frame. Reports a failure and
  // returns false.
  bool Seek(uint32_t frame);
  // Writes the frame at the cursor into the window and advances the cursor,
  // wrapping to frame 0 first when looping. Returns the status of
  // wt_anim_next_frame (WT_OK on success) or kNoWindow when there was no
  // window to draw into.
  wt_status DrawNext(wt_frame_info* frame);
  void ReportOnce(bool* reported, const std::string& code,
                  const std::string& message);

  wt_anim* const anim_;
  const uint32_t width_;
  const uint32_t height_;
  const uint32_t frame_count_;
  const bool loop_;
  const ErrorCallback on_error_;
  const ThreadHooks hooks_;

  // Guards window_. Held by the render thread for the length of one
  // lock-write-post, so SetWindow(nullptr) can't return mid-write.
  std::mutex window_mu_;
  ANativeWindow* window_ = nullptr;

  // Guards the state below and wakes the render thread.
  std::mutex mu_;
  std::condition_variable cv_;
  bool quit_ = false;
  bool active_ = false;
  bool has_window_ = false;
  // A new window arrived: draw into it before anything else.
  bool redraw_ = false;
  // SeekTo asked for seek_frame_.
  bool seek_pending_ = false;
  uint32_t seek_frame_ = 0;
  // When the shown frame went up and how long it stays; SetActive(true)
  // resumes from there.
  Clock::time_point shown_at_;
  Clock::duration shown_for_{};
  // When the next frame is due (only meaningful while active_).
  Clock::time_point next_due_;

  // Render thread only.
  // shown_index_ is the frame on screen, or the one to draw when a window
  // arrives (after a seek without one). Meaningful once drawn_any_ is set.
  bool drawn_any_ = false;
  uint32_t shown_index_ = 0;
  // Mirrors the core's cursor: the frame the next wt_anim_next_frame
  // returns. A non-looping player has ended once it reaches frame_count_.
  uint32_t cursor_ = 0;
  bool reported_decode_ = false;
  bool reported_surface_ = false;

  std::thread thread_;
};

}  // namespace wt_android

#endif  // WEBM_TEXTURE_ANDROID_SURFACE_PLAYER_H_
