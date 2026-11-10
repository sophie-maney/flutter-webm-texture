#include "surface_player.h"

#include <android/log.h>
#include <pthread.h>

#include <algorithm>
#include <utility>

#define WT_LOG(prio, ...) __android_log_print(prio, "WebmTexture", __VA_ARGS__)

namespace wt_android {
namespace {

// DrawNext found no window to draw into.
constexpr wt_status kNoWindow = 1000;

// A frame shorter than this (including 0 ms) is shown for this long, so a
// broken duration can't spin the render thread.
constexpr std::chrono::milliseconds kMinFrameTime{10};

// How long to wait before retrying a window that failed to lock.
constexpr std::chrono::milliseconds kLockRetry{100};

}  // namespace

std::unique_ptr<SurfacePlayer> SurfacePlayer::Create(
    wt_anim* anim, uint32_t width, uint32_t height, uint32_t frame_count,
    ErrorCallback on_error, ThreadHooks hooks) {
  if (anim == nullptr || width == 0 || height == 0) return nullptr;
  return std::unique_ptr<SurfacePlayer>(new SurfacePlayer(
      anim, width, height, frame_count, std::move(on_error), std::move(hooks)));
}

SurfacePlayer::SurfacePlayer(wt_anim* anim, uint32_t width, uint32_t height,
                             uint32_t frame_count, ErrorCallback on_error,
                             ThreadHooks hooks)
    : anim_(anim),
      width_(width),
      height_(height),
      frame_count_(frame_count),
      on_error_(std::move(on_error)),
      hooks_(std::move(hooks)) {
  thread_ = std::thread([this] { Run(); });
}

SurfacePlayer::~SurfacePlayer() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
  }
  cv_.notify_one();
  thread_.join();
  if (window_ != nullptr) ANativeWindow_release(window_);
  wt_anim_destroy(anim_);
}

wt_status SurfacePlayer::SetWindow(ANativeWindow* window) {
  wt_status status = WT_OK;
  if (window != nullptr) {
    const int rc = ANativeWindow_setBuffersGeometry(
        window, static_cast<int32_t>(width_), static_cast<int32_t>(height_),
        WINDOW_FORMAT_RGBA_8888);
    if (rc != 0) {
      WT_LOG(ANDROID_LOG_ERROR, "ANativeWindow_setBuffersGeometry failed: %d",
             rc);
      ANativeWindow_release(window);
      window = nullptr;
      status = WT_ERR_INTERNAL;
    }
  }
  ANativeWindow* old;
  {
    // Waits for an in-flight frame write to the old window to finish.
    std::lock_guard<std::mutex> lk(window_mu_);
    old = window_;
    window_ = window;
  }
  if (old != nullptr) ANativeWindow_release(old);
  {
    std::lock_guard<std::mutex> lk(mu_);
    has_window_ = window != nullptr;
    redraw_ = has_window_;
  }
  cv_.notify_one();
  return status;
}

void SurfacePlayer::SetActive(bool active) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (active == active_) return;
    active_ = active;
    // Resume where the shown frame's time ran out (now, if it already has).
    if (active) next_due_ = shown_at_ + shown_for_;
  }
  cv_.notify_one();
}

void SurfacePlayer::Run() {
  pthread_setname_np(pthread_self(), "wt-anim");
  if (hooks_.on_start) hooks_.on_start();
  Loop();
  if (hooks_.on_exit) hooks_.on_exit();
}

void SurfacePlayer::Loop() {
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    if (quit_) return;
    const bool can_tick = active_ && has_window_ && frame_count_ > 1;
    const bool due = can_tick && Clock::now() >= next_due_;
    if (!redraw_ && !due) {
      if (can_tick) {
        cv_.wait_until(lk, next_due_);
      } else {
        cv_.wait(lk);
      }
      continue;
    }
    const bool redraw = redraw_;
    const bool active = active_;
    redraw_ = false;
    lk.unlock();

    // A new window while paused gets the frame that was already showing. While
    // playing, the next frame is as good and costs no seek.
    if (redraw && drawn_any_ && !active) {
      const wt_status st = wt_anim_seek(anim_, shown_index_);
      if (st < 0) {
        ReportOnce(&reported_decode_, wt_status_string(st), "seek failed");
      }
    }
    const Clock::time_point start = Clock::now();
    wt_frame_info frame{};
    const wt_status st = DrawNext(&frame);

    lk.lock();
    if (st == kNoWindow) continue;
    Clock::duration hold = std::max<Clock::duration>(
        std::chrono::milliseconds(frame.duration_ms), kMinFrameTime);
    if (st == WT_OK) {
      drawn_any_ = true;
      shown_index_ = frame.index;
      shown_at_ = start;
      shown_for_ = hold;
    } else {
      hold = st == WT_ERR_IO ? kLockRetry : kMinFrameTime;
    }
    // Stay on the content clock while on time; after a stall (or a redraw),
    // restart it from this frame rather than rushing to catch up.
    const bool on_time = due && !redraw && next_due_ + hold >= start;
    next_due_ = (on_time ? next_due_ : start) + hold;
  }
}

wt_status SurfacePlayer::DrawNext(wt_frame_info* frame) {
  std::lock_guard<std::mutex> lk(window_mu_);
  if (window_ == nullptr) return kNoWindow;

  ANativeWindow_Buffer buf;
  const int rc = ANativeWindow_lock(window_, &buf, nullptr);
  if (rc != 0) {
    ReportOnce(&reported_surface_, "SURFACE_LOST",
               "ANativeWindow_lock failed: " + std::to_string(rc));
    return WT_ERR_IO;
  }
  wt_status st;
  if (buf.format != WINDOW_FORMAT_RGBA_8888 ||
      buf.width != static_cast<int32_t>(width_) ||
      buf.height != static_cast<int32_t>(height_) || buf.stride < buf.width ||
      buf.bits == nullptr) {
    ReportOnce(&reported_surface_, "SURFACE_LOST",
               "unexpected buffer: format " + std::to_string(buf.format) +
                   ", " + std::to_string(buf.width) + "x" +
                   std::to_string(buf.height) + ", stride " +
                   std::to_string(buf.stride));
    st = WT_ERR_INTERNAL;
  } else {
    const uint32_t stride = static_cast<uint32_t>(buf.stride) * 4;
    const size_t size = static_cast<size_t>(stride) * (height_ - 1) +
                        static_cast<size_t>(width_) * 4;
    auto* dst = static_cast<uint8_t*>(buf.bits);
    st = wt_anim_next_frame(anim_, dst, size, stride, WT_PIXEL_FORMAT_RGBA8888,
                            frame);
    if (st == WT_END) {
      st = wt_anim_seek(anim_, 0);
      if (st == WT_OK) {
        st = wt_anim_next_frame(anim_, dst, size, stride,
                                WT_PIXEL_FORMAT_RGBA8888, frame);
      }
    }
    if (st == WT_OK) {
      reported_surface_ = false;
    } else {
      // The core advanced past the broken frame; keep playing.
      ReportOnce(&reported_decode_, wt_status_string(st),
                 "frame decode failed");
    }
  }
  // A locked buffer must always be posted; there is no way to cancel it.
  ANativeWindow_unlockAndPost(window_);
  return st;
}

void SurfacePlayer::ReportOnce(bool* reported, const std::string& code,
                               const std::string& message) {
  if (*reported) return;
  *reported = true;
  WT_LOG(ANDROID_LOG_WARN, "%s: %s", code.c_str(), message.c_str());
  if (on_error_) on_error_(code, message);
}

}  // namespace wt_android
