// JNI entry points for com.example.webm_texture.WtNative (T2.2). Compiled
// into libwebm_texture.so next to the core, so Kotlin and dart:ffi share one
// copy of it.
//
// Thread affinity matches SurfacePlayer: nativeCreate on a background thread,
// everything else on the platform main thread.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <memory>
#include <string>

#include "surface_player.h"
#include "wt_api.h"

namespace {

using wt_android::SurfacePlayer;

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

  // Must run after the player (and so its render thread) is gone.
  void Release(JNIEnv* env) {
    if (listener_ != nullptr) env->DeleteGlobalRef(listener_);
    listener_ = nullptr;
  }

  // Attaches the calling (render) thread to the JVM for its lifetime.
  wt_android::ThreadHooks Hooks() const {
    JavaVM* vm = vm_;
    return {[vm] {
              JNIEnv* env = nullptr;
              JavaVMAttachArgs args{JNI_VERSION_1_6, "wt-anim", nullptr};
              vm->AttachCurrentThread(&env, &args);
            },
            [vm] { vm->DetachCurrentThread(); }};
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
// source size) and its render thread, looping or stopping on the last frame.
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

  auto handle = std::make_unique<Handle>();
  handle->relay = std::make_unique<ErrorRelay>(env, listener);
  const ErrorRelay* relay = handle->relay.get();
  handle->player = SurfacePlayer::Create(
      anim, out_w, out_h, info.frame_count, loop == JNI_TRUE,
      [relay](const std::string& code, const std::string& message) {
        relay->Send(code, message);
      },
      relay->Hooks());
  if (handle->player == nullptr) {
    wt_anim_destroy(anim);
    handle->relay->Release(env);
    SetOut(env, out, WT_ERR_INTERNAL);
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

// Shows frame next (see SurfacePlayer::SeekTo). Returns a wt_status:
// WT_ERR_INVALID_ARGUMENT for a frame outside the animation.
// Thread: platform main thread.
JNIEXPORT jint JNICALL Java_com_example_webm_1texture_WtNative_nativeSeek(
    JNIEnv*, jclass, jlong handle, jint frame) {
  Handle* h = FromJ(handle);
  if (h == nullptr || frame < 0) return WT_ERR_INVALID_ARGUMENT;
  return h->player->SeekTo(static_cast<uint32_t>(frame));
}

// Stops the render thread, releases the window and the decoder. Waits for at
// most one in-flight frame.
// Thread: platform main thread.
JNIEXPORT void JNICALL Java_com_example_webm_1texture_WtNative_nativeDestroy(
    JNIEnv* env, jclass, jlong handle) {
  Handle* h = FromJ(handle);
  if (h == nullptr) return;
  h->player.reset();
  h->relay->Release(env);
  delete h;
}

// The core's name for a status code, e.g. "WT_ERR_LIMIT".
// Thread: any thread.
JNIEXPORT jstring JNICALL
Java_com_example_webm_1texture_WtNative_nativeStatusName(JNIEnv* env, jclass,
                                                         jint status) {
  return env->NewStringUTF(wt_status_string(status));
}

}  // extern "C"
