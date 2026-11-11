#include "atlas_window.h"

#include <android/log.h>

#define WT_LOG(prio, ...) __android_log_print(prio, "WebmTexture", __VA_ARGS__)

namespace wt_android {

std::unique_ptr<AtlasWindow> AtlasWindow::Create(ANativeWindow* window,
                                                 uint32_t width,
                                                 uint32_t height) {
  if (window == nullptr) return nullptr;
  const int rc = ANativeWindow_setBuffersGeometry(
      window, static_cast<int32_t>(width), static_cast<int32_t>(height),
      WINDOW_FORMAT_RGBA_8888);
  if (rc != 0) {
    WT_LOG(ANDROID_LOG_ERROR,
           "atlas page: ANativeWindow_setBuffersGeometry(%u, %u) failed: %d",
           width, height, rc);
    ANativeWindow_release(window);
    return nullptr;
  }
  return std::unique_ptr<AtlasWindow>(new AtlasWindow(window, width, height));
}

AtlasWindow::~AtlasWindow() { ANativeWindow_release(window_); }

wt_atlas_target AtlasWindow::target() {
  wt_atlas_target t{};
  t.context = this;
  t.acquire = &AtlasWindow::Acquire;
  t.present = &AtlasWindow::Present;
  return t;
}

wt_status AtlasWindow::Acquire(void* context, wt_buffer* out, uint64_t* id) {
  auto* self = static_cast<AtlasWindow*>(context);
  ANativeWindow_Buffer buf;
  const int rc = ANativeWindow_lock(self->window_, &buf, nullptr);
  if (rc != 0) {
    if (!self->reported_) {
      self->reported_ = true;
      WT_LOG(ANDROID_LOG_WARN, "atlas page: ANativeWindow_lock failed: %d", rc);
    }
    return WT_ERR_IO;  // The core retries in about 100 ms.
  }
  if (buf.format != WINDOW_FORMAT_RGBA_8888 ||
      buf.width != static_cast<int32_t>(self->width_) ||
      buf.height != static_cast<int32_t>(self->height_) ||
      buf.stride < buf.width || buf.bits == nullptr) {
    WT_LOG(ANDROID_LOG_ERROR,
           "atlas page: unexpected buffer: format %d, %dx%d, stride %d",
           buf.format, buf.width, buf.height, buf.stride);
    ANativeWindow_unlockAndPost(self->window_);
    return WT_ERR_INTERNAL;
  }
  const uint32_t stride = static_cast<uint32_t>(buf.stride) * 4;
  out->pixels = static_cast<uint8_t*>(buf.bits);
  out->size = static_cast<size_t>(stride) * (self->height_ - 1) +
              static_cast<size_t>(self->width_) * 4;
  out->stride = stride;
  out->format = WT_PIXEL_FORMAT_RGBA8888;
  *id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buf.bits));
  return WT_OK;
}

void AtlasWindow::Present(void* context, wt_status status) {
  auto* self = static_cast<AtlasWindow*>(context);
  ANativeWindow_unlockAndPost(self->window_);
  if (status == WT_OK) self->reported_ = false;
}

}  // namespace wt_android
