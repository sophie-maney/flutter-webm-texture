// Spike B (T0.8): fill a SurfaceProducer's Surface from native code through
// ANativeWindow_lock, as the real plugin will (PLAN.md, T2.2).
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdint>
#include <cstring>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "SpikeB", __VA_ARGS__)

namespace {

// Locks the window, calls fill(row pointer, y) for each row, posts.
// Returns 0 on success or the negative status of the failing call.
template <typename F>
int DrawRows(ANativeWindow* win, int w, int h, F fill) {
  ANativeWindow_Buffer buf;
  int rc = ANativeWindow_lock(win, &buf, nullptr);
  if (rc != 0) {
    LOG("ANativeWindow_lock failed: %d", rc);
    return rc;
  }
  if (buf.format != WINDOW_FORMAT_RGBA_8888 || buf.width < w ||
      buf.height < h) {
    LOG("unexpected buffer: format=%d %dx%d stride=%d", buf.format, buf.width,
        buf.height, buf.stride);
  }
  const int rows = buf.height < h ? buf.height : h;
  const int cols = buf.width < w ? buf.width : w;
  auto* base = static_cast<uint8_t*>(buf.bits);
  for (int y = 0; y < rows; ++y) {
    fill(base + static_cast<size_t>(y) * buf.stride * 4, y, cols);
  }
  rc = ANativeWindow_unlockAndPost(win);
  if (rc != 0) LOG("ANativeWindow_unlockAndPost failed: %d", rc);
  return rc;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_dev_webmtexture_spike_1b_SpikeNative_attach(
    JNIEnv* env, jclass, jobject surface, jint w, jint h) {
  ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
  if (win == nullptr) {
    LOG("ANativeWindow_fromSurface returned null");
    return 0;
  }
  int rc = ANativeWindow_setBuffersGeometry(win, w, h, WINDOW_FORMAT_RGBA_8888);
  LOG("attach %p %dx%d setBuffersGeometry=%d format(before lock)=%d", win, w,
      h, rc, ANativeWindow_getFormat(win));
  return reinterpret_cast<jlong>(win);
}

JNIEXPORT void JNICALL Java_dev_webmtexture_spike_1b_SpikeNative_release(
    JNIEnv*, jclass, jlong handle) {
  if (handle != 0) {
    ANativeWindow_release(reinterpret_cast<ANativeWindow*>(handle));
  }
}

// Copies a tightly packed premultiplied RGBA image into the window.
JNIEXPORT jint JNICALL Java_dev_webmtexture_spike_1b_SpikeNative_drawRgba(
    JNIEnv* env, jclass, jlong handle, jbyteArray rgba, jint w, jint h) {
  auto* win = reinterpret_cast<ANativeWindow*>(handle);
  if (win == nullptr) return -1;
  const jsize len = env->GetArrayLength(rgba);
  if (len < w * h * 4) return -2;
  jbyte* src = env->GetByteArrayElements(rgba, nullptr);
  int rc = DrawRows(win, w, h, [&](uint8_t* row, int y, int cols) {
    std::memcpy(row, src + static_cast<size_t>(y) * w * 4, cols * 4);
  });
  env->ReleaseByteArrayElements(rgba, src, JNI_ABORT);
  return rc;
}

// Animated pattern: whole texture is premultiplied red with alpha cycling
// with the frame index; the top 16 px show the frame index in binary
// (8 blocks, opaque white = 1, opaque black = 0), so screenshots prove
// frames keep coming.
JNIEXPORT jint JNICALL Java_dev_webmtexture_spike_1b_SpikeNative_drawAnim(
    JNIEnv*, jclass, jlong handle, jint w, jint h, jint frame) {
  auto* win = reinterpret_cast<ANativeWindow*>(handle);
  if (win == nullptr) return -1;
  const uint8_t a = static_cast<uint8_t>((frame * 4) & 0xff);
  const int bw = w / 8;
  return DrawRows(win, w, h, [&](uint8_t* row, int y, int cols) {
    for (int x = 0; x < cols; ++x) {
      uint8_t* p = row + x * 4;
      if (y < 16) {
        const int bit = 7 - x / bw;
        const uint8_t v = ((frame >> bit) & 1) ? 255 : 0;
        p[0] = p[1] = p[2] = v;
        p[3] = 255;
      } else {
        p[0] = a;
        p[1] = 0;
        p[2] = 0;
        p[3] = a;
      }
    }
  });
}

}  // extern "C"
