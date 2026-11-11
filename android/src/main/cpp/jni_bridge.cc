// JNI entry points for com.example.webm_texture.WtNative (T2.2). Compiled
// into libwebm_texture.so next to the core, so Kotlin and dart:ffi share one
// copy of it.
//
// Every animation plays on one process-wide scheduler (T3.3), created by the
// first nativeCreate. Its threads are attached to the JVM for their lifetime
// (ADR 0011: ImageReader would otherwise attach and detach them around every
// posted frame).
//
// Thread affinity matches SurfacePlayer: nativeCreate on a background thread,
// everything else on the platform main thread.
//
// Atlases (T4.2, ADR 0020): nativeAtlas* wrap wt_atlas_*, and each page's
// SurfaceProducer surface becomes an AtlasWindow, its wt_atlas_target.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include "atlas_window.h"
#include "surface_player.h"
#include "wt_api.h"

namespace {

using wt_android::AtlasWindow;
using wt_android::SurfacePlayer;

// Out-array layout of nativeAtlasAttach.
enum : jsize {
  kSlotPage = 0,
  kSlotX = 1,
  kSlotY = 2,
  kSlotWidth = 3,
  kSlotHeight = 4,
  kSlotLength = 5,
};

// Out-array layout of nativeAtlasPageInfo (wt_atlas_page_info).
enum : jsize {
  kPageWidth = 0,
  kPageUsedHeight = 1,
  kPageWantedHeight = 2,
  kPageHeight = 3,
  kPageHasTarget = 4,
  kPageLiveSlots = 5,
  kPageBuffers = 6,
  kPageStride = 7,
  kPageBufferBytes = 8,
  kPageCpuBytes = 9,
  kPagePresents = 10,
  kPageFailures = 11,
  kPageLength = 12,
};

// What an atlas jlong handle points at: the core's atlas and the window each
// page presents into (nullptr while it has none).
struct AtlasHandle {
  wt_atlas* atlas = nullptr;
  uint32_t width = 0;
  std::vector<std::unique_ptr<AtlasWindow>> windows;
};

AtlasHandle* AtlasFromJ(jlong handle) {
  return reinterpret_cast<AtlasHandle*>(static_cast<intptr_t>(handle));
}

// Out-array layout of nativeCreate.
enum : jsize {
  kOutStatus = 0,
  kOutWidth = 1,
  kOutHeight = 2,
  kOutFrames = 3,
  kOutLength = 4,
};

// Delivers render-thread errors to listener.onNativeError(String, String).
class ErrorRelay {
 public:
  ErrorRelay(JNIEnv* env, jobject listener) {
    env->GetJavaVM(&vm_);
    listener_ = env->NewGlobalRef(listener);
    jclass cls = env->GetObjectClass(listener);
    method_ = env->GetMethodID(cls, "onNativeError",
                               "(Ljava/lang/String;Ljava/lang/String;)V");
    env->DeleteLocalRef(cls);
    if (method_ == nullptr) env->ExceptionClear();
  }

  // Must run after the player is gone, so no worker can still call Send.
  void Release(JNIEnv* env) {
    if (listener_ != nullptr) env->DeleteGlobalRef(listener_);
    listener_ = nullptr;
  }

  void Send(const std::string& code, const std::string& message) const {
    if (listener_ == nullptr || method_ == nullptr) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) !=
        JNI_OK) {
      if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
      attached = true;
    }
    jstring jcode = env->NewStringUTF(code.c_str());
    jstring jmessage = env->NewStringUTF(message.c_str());
    if (jcode != nullptr && jmessage != nullptr) {
      env->CallVoidMethod(listener_, method_, jcode, jmessage);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (jcode != nullptr) env->DeleteLocalRef(jcode);
    if (jmessage != nullptr) env->DeleteLocalRef(jmessage);
    if (attached) vm_->DetachCurrentThread();
  }

 private:
  JavaVM* vm_ = nullptr;
  jobject listener_ = nullptr;
  jmethodID method_ = nullptr;
};

// Attaches each scheduler thread to the JVM under its own name.
void AttachThread(void* vm, const char* name) {
  JNIEnv* env = nullptr;
  JavaVMAttachArgs args{JNI_VERSION_1_6, name, nullptr};
  static_cast<JavaVM*>(vm)->AttachCurrentThread(&env, &args);
}

void DetachThread(void* vm) { static_cast<JavaVM*>(vm)->DetachCurrentThread(); }

// The process-wide scheduler, or nullptr if it couldn't start. It lives as
// long as the process. setSchedulerOptions applies the device tier to it.
wt_sched* SharedScheduler(JNIEnv* env) {
  static wt_sched* const sched = [env] {
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) return static_cast<wt_sched*>(nullptr);
    wt_sched_hooks hooks{};
    hooks.context = vm;
    hooks.thread_start = &AttachThread;
    hooks.thread_stop = &DetachThread;
    wt_sched* s = nullptr;
    const wt_status st = wt_sched_create(nullptr, &hooks, &s);
    if (st != WT_OK) {
      __android_log_print(ANDROID_LOG_ERROR, "WebmTexture",
                          "wt_sched_create failed: %s", wt_status_string(st));
    }
    return s;
  }();
  return sched;
}

// What a jlong handle points at.
struct Handle {
  std::unique_ptr<ErrorRelay> relay;
  std::unique_ptr<SurfacePlayer> player;
};

Handle* FromJ(jlong handle) {
  return reinterpret_cast<Handle*>(static_cast<intptr_t>(handle));
}

void SetOut(JNIEnv* env, jintArray out, wt_status status, uint32_t w = 0,
            uint32_t h = 0, uint32_t frames = 0) {
  const jint values[kOutLength] = {status, static_cast<jint>(w),
                                   static_cast<jint>(h),
                                   static_cast<jint>(frames)};
  env->SetIntArrayRegion(out, 0, kOutLength, values);
}

wt_status CreateSource(JNIEnv* env, jbyteArray bytes, jstring path,
                       wt_source** out) {
  if (bytes != nullptr) {
    const jsize size = env->GetArrayLength(bytes);
    if (size <= 0) return WT_ERR_INVALID_ARGUMENT;
    // wt_source_create_memory checks the size against the limits before it
    // copies, so the critical section is bounded by max_bytes.
    void* data = env->GetPrimitiveArrayCritical(bytes, nullptr);
    if (data == nullptr) return WT_ERR_NO_MEMORY;
    const wt_status st =
        wt_source_create_memory(static_cast<const uint8_t*>(data),
                                static_cast<size_t>(size), nullptr, out);
    env->ReleasePrimitiveArrayCritical(bytes, data, JNI_ABORT);
    return st;
  }
  if (path != nullptr) {
    const char* utf = env->GetStringUTFChars(path, nullptr);
    if (utf == nullptr) return WT_ERR_NO_MEMORY;
    const wt_status st = wt_source_create_file(utf, nullptr, out);
    env->ReleaseStringUTFChars(path, utf);
    return st;
  }
  return WT_ERR_INVALID_ARGUMENT;
}

// The cache file for source_key at width x height inside cache_dir, or "" when
// there is nothing to cache: no directory or key, or a size that isn't known
// until the source is probed (0 x 0).
std::string CachePath(JNIEnv* env, jstring cache_dir, jstring source_key,
                      jint width, jint height) {
  if (cache_dir == nullptr || source_key == nullptr || width <= 0 ||
      height <= 0) {
    return "";
  }
  const char* dir = env->GetStringUTFChars(cache_dir, nullptr);
  if (dir == nullptr) return "";
  const char* key = env->GetStringUTFChars(source_key, nullptr);
  if (key == nullptr) {
    env->ReleaseStringUTFChars(cache_dir, dir);
    return "";
  }
  char path[4096];
  const wt_status st =
      wt_cache_file_path(dir, key, static_cast<uint32_t>(width),
                         static_cast<uint32_t>(height), path, sizeof(path));
  env->ReleaseStringUTFChars(source_key, key);
  env->ReleaseStringUTFChars(cache_dir, dir);
  return st == WT_OK ? std::string(path) : std::string();
}

}  // namespace

extern "C" {

// Deletes least-recently-used cache files in cache_dir down to the disk
// budget, and stray temporaries. Returns a wt_status.
// Thread: background thread (reads the directory).
JNIEXPORT jint JNICALL Java_com_example_webm_1texture_WtNative_nativeEvictCache(
    JNIEnv* env, jclass, jstring cache_dir) {
  if (cache_dir == nullptr) return WT_ERR_INVALID_ARGUMENT;
  const char* dir = env->GetStringUTFChars(cache_dir, nullptr);
  if (dir == nullptr) return WT_ERR_NO_MEMORY;
  const wt_status st = wt_cache_evict(dir, nullptr);
  env->ReleaseStringUTFChars(cache_dir, dir);
  return st;
}

// Creates the decoder for bytes or path at width x height (0 x 0 for the
// source size) and puts it on the shared scheduler, looping or stopping on
// the last frame.
// With a cache_dir and a source_key (and a non-zero size) frames are cached in
// a file there, named by wt_cache_file_path; otherwise nothing is cached.
// Writes {status, outWidth, outHeight, frameCount} into out and returns the
// handle, or 0 on failure. Thread: background thread.
JNIEXPORT jlong JNICALL Java_com_example_webm_1texture_WtNative_nativeCreate(
    JNIEnv* env, jclass, jobject listener, jbyteArray bytes, jstring path,
    jint width, jint height, jboolean loop, jstring cache_dir,
    jstring source_key, jintArray out) {
  if (out == nullptr || env->GetArrayLength(out) < kOutLength) return 0;
  if (listener == nullptr || width < 0 || height < 0) {
    SetOut(env, out, WT_ERR_INVALID_ARGUMENT);
    return 0;
  }
  wt_source* source = nullptr;
  wt_status st = CreateSource(env, bytes, path, &source);
  if (st != WT_OK) {
    SetOut(env, out, st);
    return 0;
  }
  wt_anim* anim = nullptr;
  const std::string cache_path =
      CachePath(env, cache_dir, source_key, width, height);
  st = wt_anim_create_cached(
      source, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
      cache_path.empty() ? nullptr : cache_path.c_str(), &anim);
  wt_source_destroy(source);  // The anim holds its own reference.
  if (st != WT_OK) {
    SetOut(env, out, st);
    return 0;
  }
  wt_info info{};
  uint32_t out_w = 0;
  uint32_t out_h = 0;
  st = wt_anim_get_info(anim, &info, &out_w, &out_h);
  if (st != WT_OK) {
    wt_anim_destroy(anim);
    SetOut(env, out, st);
    return 0;
  }

  wt_sched* sched = SharedScheduler(env);
  if (sched == nullptr) {
    wt_anim_destroy(anim);
    SetOut(env, out, WT_ERR_NO_MEMORY);
    return 0;
  }
  auto handle = std::make_unique<Handle>();
  handle->relay = std::make_unique<ErrorRelay>(env, listener);
  const ErrorRelay* relay = handle->relay.get();
  handle->player = SurfacePlayer::Create(
      sched, anim, out_w, out_h, loop == JNI_TRUE,
      [relay](const std::string& code, const std::string& message) {
        relay->Send(code, message);
      },
      &st);
  if (handle->player == nullptr) {
    wt_anim_destroy(anim);  // Still ours when Create fails.
    handle->relay->Release(env);
    SetOut(env, out, st != WT_OK ? st : WT_ERR_INTERNAL);
    return 0;
  }
  SetOut(env, out, WT_OK, out_w, out_h, info.frame_count);
  return static_cast<jlong>(reinterpret_cast<intptr_t>(handle.release()));
}

// Points the player at surface (null releases the current window before
// returning). Returns a wt_status.
// Thread: platform main thread.
JNIEXPORT jint JNICALL Java_com_example_webm_1texture_WtNative_nativeSetSurface(
    JNIEnv* env, jclass, jlong handle, jobject surface) {
  Handle* h = FromJ(handle);
  if (h == nullptr) return WT_ERR_INVALID_ARGUMENT;
  ANativeWindow* window = nullptr;
  if (surface != nullptr) {
    window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) return WT_ERR_INTERNAL;
  }
  return h->player->SetWindow(window);
}

// Thread: platform main thread.
JNIEXPORT void JNICALL Java_com_example_webm_1texture_WtNative_nativeSetActive(
    JNIEnv*, jclass, jlong handle, jboolean active) {
  Handle* h = FromJ(handle);
  if (h != nullptr) h->player->SetActive(active == JNI_TRUE);
}

// The on-screen area of the animation's widgets in physical pixels, its rank
// under the scheduler's max_concurrent limit (see SurfacePlayer::SetPriority).
// Thread: platform main thread.
JNIEXPORT void JNICALL
Java_com_example_webm_1texture_WtNative_nativeSetPriority(JNIEnv*, jclass,
                                                          jlong handle,
                                                          jlong priority) {
  Handle* h = FromJ(handle);
  if (h != nullptr && priority >= 0) {
    h->player->SetPriority(static_cast<uint64_t>(priority));
  }
}

// Shows frame next (see SurfacePlayer::SeekTo). Returns a wt_status:
// WT_ERR_INVALID_ARGUMENT for a frame outside the animation.
// Thread: platform main thread.
JNIEXPORT jint JNICALL Java_com_example_webm_1texture_WtNative_nativeSeek(
    JNIEnv*, jclass, jlong handle, jint frame) {
  Handle* h = FromJ(handle);
  if (h == nullptr || frame < 0) return WT_ERR_INVALID_ARGUMENT;
  return h->player->SeekTo(static_cast<uint32_t>(frame));
}

// Takes the player off the scheduler, releases the window and the decoder.
// Waits for at most one in-flight frame.
// Thread: platform main thread.
JNIEXPORT void JNICALL Java_com_example_webm_1texture_WtNative_nativeDestroy(
    JNIEnv* env, jclass, jlong handle) {
  Handle* h = FromJ(handle);
  if (h == nullptr) return;
  h->player.reset();
  h->relay->Release(env);
  delete h;
}

// Applies the device tier's tunables to the shared scheduler (creating it if
// needed); running animations follow on the next round. Returns a wt_status:
// WT_ERR_INVALID_ARGUMENT for a value out of range.
// Thread: platform main thread.
JNIEXPORT jint JNICALL
Java_com_example_webm_1texture_WtNative_nativeSetSchedulerOptions(
    JNIEnv* env, jclass, jint worker_count, jint max_fps, jint max_concurrent) {
  if (worker_count < 1 || max_fps < 1 || max_concurrent < 0) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  wt_sched* sched = SharedScheduler(env);
  if (sched == nullptr) return WT_ERR_NO_MEMORY;
  wt_sched_options options;
  wt_sched_options_init_default(&options);
  options.worker_count = static_cast<uint32_t>(worker_count);
  options.max_fps = static_cast<uint32_t>(max_fps);
  options.max_concurrent = static_cast<uint32_t>(max_concurrent);
  return wt_sched_set_options(sched, &options);
}

// Creates an empty atlas on the shared scheduler with pages `width` wide
// (rounded down to 16) and up to max_height tall, at most max_pages of them,
// in RGBA_8888. Writes the status into out[0] and returns the handle, or 0.
// Thread: platform main thread.
JNIEXPORT jlong JNICALL
Java_com_example_webm_1texture_WtNative_nativeAtlasCreate(JNIEnv* env, jclass,
                                                          jint width,
                                                          jint max_height,
                                                          jint max_pages,
                                                          jintArray out) {
  if (out == nullptr || env->GetArrayLength(out) < 1) return 0;
  jint status = WT_ERR_INVALID_ARGUMENT;
  wt_sched* sched = SharedScheduler(env);
  if (sched == nullptr) status = WT_ERR_NO_MEMORY;
  std::unique_ptr<AtlasHandle> h;
  if (sched != nullptr && width > 0 && max_height > 0 && max_pages > 0) {
    wt_atlas_options options;
    wt_atlas_options_init_default(&options);
    options.width = static_cast<uint32_t>(width);
    options.max_height = static_cast<uint32_t>(max_height);
    options.max_pages = static_cast<uint32_t>(max_pages);
    options.format = WT_PIXEL_FORMAT_RGBA8888;
    h = std::make_unique<AtlasHandle>();
    h->width = options.width & ~15u;
    status = wt_atlas_create(sched, &options, &h->atlas);
  }
  env->SetIntArrayRegion(out, 0, 1, &status);
  if (status != WT_OK) return 0;
  return static_cast<jlong>(reinterpret_cast<intptr_t>(h.release()));
}

// Gives every animation in the atlas back its own sink, waits for presents
// in flight, then releases every page's window. The caller releases the
// SurfaceProducers afterwards. Thread: platform main thread.
JNIEXPORT void JNICALL
Java_com_example_webm_1texture_WtNative_nativeAtlasDestroy(JNIEnv*, jclass,
                                                           jlong atlas) {
  AtlasHandle* h = AtlasFromJ(atlas);
  if (h == nullptr) return;
  wt_atlas_destroy(h->atlas);  // No target callback runs after this.
  delete h;
}

// Moves the animation into a slot of the atlas (wt_atlas_attach). Writes
// {page, x, y, width, height} into out and returns a wt_status.
// Thread: platform main thread (waits for at most one frame in flight).
JNIEXPORT jint JNICALL
Java_com_example_webm_1texture_WtNative_nativeAtlasAttach(JNIEnv* env, jclass,
                                                          jlong atlas,
                                                          jlong anim,
                                                          jintArray out) {
  AtlasHandle* a = AtlasFromJ(atlas);
  Handle* p = FromJ(anim);
  if (a == nullptr || p == nullptr || out == nullptr ||
      env->GetArrayLength(out) < kSlotLength) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  wt_atlas_slot slot;
  const wt_status st = wt_atlas_attach(a->atlas, p->player->player(), &slot);
  if (st != WT_OK) return st;
  const jint values[kSlotLength] = {
      static_cast<jint>(slot.page), static_cast<jint>(slot.x),
      static_cast<jint>(slot.y), static_cast<jint>(slot.width),
      static_cast<jint>(slot.height)};
  env->SetIntArrayRegion(out, 0, kSlotLength, values);
  return WT_OK;
}

// Moves the animation back to its own sink (wt_atlas_detach). Returns a
// wt_status. Thread: platform main thread (waits for at most one frame).
JNIEXPORT jint JNICALL
Java_com_example_webm_1texture_WtNative_nativeAtlasDetach(JNIEnv*, jclass,
                                                          jlong atlas,
                                                          jlong anim) {
  AtlasHandle* a = AtlasFromJ(atlas);
  Handle* p = FromJ(anim);
  if (a == nullptr || p == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return wt_atlas_detach(a->atlas, p->player->player());
}

// Pages created so far. Thread: platform main thread.
JNIEXPORT jint JNICALL
Java_com_example_webm_1texture_WtNative_nativeAtlasPageCount(JNIEnv*, jclass,
                                                             jlong atlas) {
  AtlasHandle* a = AtlasFromJ(atlas);
  return a == nullptr ? 0 : static_cast<jint>(wt_atlas_page_count(a->atlas));
}

// Fills out with wt_atlas_page_info (see the kPage* layout). Returns a
// wt_status. Thread: platform main thread; never waits for a frame.
JNIEXPORT jint JNICALL
Java_com_example_webm_1texture_WtNative_nativeAtlasPageInfo(JNIEnv* env, jclass,
                                                            jlong atlas,
                                                            jint page,
                                                            jlongArray out) {
  AtlasHandle* a = AtlasFromJ(atlas);
  if (a == nullptr || page < 0 || out == nullptr ||
      env->GetArrayLength(out) < kPageLength) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  wt_atlas_page_info info;
  const wt_status st =
      wt_atlas_get_page(a->atlas, static_cast<uint32_t>(page), &info);
  if (st != WT_OK) return st;
  const jlong values[kPageLength] = {
      info.width,
      info.used_height,
      info.wanted_height,
      info.height,
      info.has_target,
      info.live_slots,
      info.buffers,
      info.buffer_stride,
      static_cast<jlong>(info.buffer_bytes),
      static_cast<jlong>(info.cpu_bytes),
      static_cast<jlong>(info.presents),
      static_cast<jlong>(info.present_failures),
  };
  env->SetLongArrayRegion(out, 0, kPageLength, values);
  return WT_OK;
}

// Points page `page` at surface, a SurfaceProducer surface of the page's
// width x height (null: no surface for now, keeping height; height 0: none
// at all). Waits for a present in flight on the old window, then releases
// it. Returns a wt_status. Thread: platform main thread.
JNIEXPORT jint JNICALL
Java_com_example_webm_1texture_WtNative_nativeAtlasSetSurface(
    JNIEnv* env, jclass, jlong atlas, jint page, jobject surface, jint height) {
  AtlasHandle* a = AtlasFromJ(atlas);
  if (a == nullptr || page < 0 || height < 0) return WT_ERR_INVALID_ARGUMENT;
  const auto index = static_cast<uint32_t>(page);
  if (index >= wt_atlas_page_count(a->atlas)) return WT_ERR_INVALID_ARGUMENT;
  std::unique_ptr<AtlasWindow> next;
  if (surface != nullptr) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) return WT_ERR_INTERNAL;
    next = AtlasWindow::Create(window, a->width, static_cast<uint32_t>(height));
    if (next == nullptr) return WT_ERR_INTERNAL;
  }
  wt_atlas_target target{};
  if (next != nullptr) target = next->target();
  const wt_status st =
      wt_atlas_set_target(a->atlas, index, next != nullptr ? &target : nullptr,
                          static_cast<uint32_t>(height));
  if (st != WT_OK) return st;  // `next` is released; the old one stays.
  if (a->windows.size() <= index) a->windows.resize(index + 1);
  // The core no longer calls the old window: release it.
  a->windows[index] = std::move(next);
  return WT_OK;
}

// The core's name for a status code, e.g. "WT_ERR_LIMIT".
// Thread: any thread.
JNIEXPORT jstring JNICALL
Java_com_example_webm_1texture_WtNative_nativeStatusName(JNIEnv* env, jclass,
                                                         jint status) {
  return env->NewStringUTF(wt_status_string(status));
}

}  // extern "C"
