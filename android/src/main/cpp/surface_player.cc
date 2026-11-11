#include "surface_player.h"

#include <android/log.h>

#include <utility>

#define WT_LOG(prio, ...) __android_log_print(prio, "WebmTexture", __VA_ARGS__)

namespace wt_android {

std::unique_ptr<SurfacePlayer> SurfacePlayer::Create(
    wt_sched* sched, wt_anim* anim, uint32_t width, uint32_t height, bool loop,
    ErrorCallback on_error, wt_status* out_status) {
  *out_status = WT_ERR_INVALID_ARGUMENT;
  if (sched == nullptr || anim == nullptr || width == 0 || height == 0) {
    return nullptr;
  }
  std::unique_ptr<SurfacePlayer> player(
      new SurfacePlayer(width, height, std::move(on_error)));
  wt_sink sink{};
  sink.context = player.get();
  sink.acquire = &SurfacePlayer::Acquire;
  sink.release = &SurfacePlayer::Release;
  sink.error = &SurfacePlayer::Error;
  // From here the workers may call the sink, which only needs the members
  // set above: there is no window yet, so the first frame waits for one.
  *out_status = wt_player_create(sched, anim, loop, &sink, &player->player_);
  if (*out_status != WT_OK) return nullptr;
  return player;
}

SurfacePlayer::SurfacePlayer(uint32_t width, uint32_t height,
                             ErrorCallback on_error)
    : width_(width), height_(height), on_error_(std::move(on_error)) {}

SurfacePlayer::~SurfacePlayer() {
  // Waits for a frame in flight; no sink callback runs after this.
  wt_player_destroy(player_);
  if (window_ != nullptr) ANativeWindow_release(window_);
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
  // A new window gets the current frame; without one the scheduler parks
  // the player at its next attempt (WT_NO_TARGET).
  if (window != nullptr) wt_player_redraw(player_);
  return status;
}

void SurfacePlayer::SetActive(bool active) {
  wt_player_set_active(player_, active);
}

void SurfacePlayer::SetPriority(uint64_t priority) {
  wt_player_set_priority(player_, priority);
}

wt_status SurfacePlayer::SeekTo(uint32_t frame) {
  return wt_player_seek(player_, frame);
}

wt_status SurfacePlayer::Acquire(void* context, wt_buffer* out) {
  auto* self = static_cast<SurfacePlayer*>(context);
  // Unlocked in Release, or below when no buffer is handed out.
  self->window_mu_.lock();
  if (self->window_ == nullptr) {
    self->window_mu_.unlock();
    return WT_NO_TARGET;
  }
  ANativeWindow_Buffer buf;
  const int rc = ANativeWindow_lock(self->window_, &buf, nullptr);
  if (rc != 0) {
    self->ReportSurfaceOnce("ANativeWindow_lock failed: " + std::to_string(rc));
    self->window_mu_.unlock();
    return WT_ERR_IO;  // The scheduler retries in about 100 ms.
  }
  if (buf.format != WINDOW_FORMAT_RGBA_8888 ||
      buf.width != static_cast<int32_t>(self->width_) ||
      buf.height != static_cast<int32_t>(self->height_) ||
      buf.stride < buf.width || buf.bits == nullptr) {
    self->ReportSurfaceOnce(
        "unexpected buffer: format " + std::to_string(buf.format) + ", " +
        std::to_string(buf.width) + "x" + std::to_string(buf.height) +
        ", stride " + std::to_string(buf.stride));
    // A locked buffer must always be posted; there is no way to cancel it.
    ANativeWindow_unlockAndPost(self->window_);
    self->window_mu_.unlock();
    return WT_ERR_INTERNAL;
  }
  const uint32_t stride = static_cast<uint32_t>(buf.stride) * 4;
  out->pixels = static_cast<uint8_t*>(buf.bits);
  out->size = static_cast<size_t>(stride) * (self->height_ - 1) +
              static_cast<size_t>(self->width_) * 4;
  out->stride = stride;
  out->format = WT_PIXEL_FORMAT_RGBA8888;
  return WT_OK;  // window_mu_ stays locked until Release.
}

void SurfacePlayer::Release(void* context, wt_status status,
                            const wt_frame_info* /*frame*/) {
  auto* self = static_cast<SurfacePlayer*>(context);
  // Posted even after a failed decode: a locked buffer can't be cancelled.
  // The core reports that failure through Error.
  ANativeWindow_unlockAndPost(self->window_);
  if (status == WT_OK) self->reported_surface_ = false;
  self->window_mu_.unlock();
}

void SurfacePlayer::Error(void* context, wt_status status) {
  auto* self = static_cast<SurfacePlayer*>(context);
  WT_LOG(ANDROID_LOG_WARN, "%s: frame decode failed", wt_status_string(status));
  if (self->on_error_) {
    self->on_error_(wt_status_string(status), "frame decode failed");
  }
}

void SurfacePlayer::ReportSurfaceOnce(const std::string& message) {
  if (reported_surface_) return;
  reported_surface_ = true;
  WT_LOG(ANDROID_LOG_WARN, "SURFACE_LOST: %s", message.c_str());
  if (on_error_) on_error_("SURFACE_LOST", message);
}

}  // namespace wt_android
