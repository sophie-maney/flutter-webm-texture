// Spike C (T0.9): fill N standalone SurfaceProducer windows, or one atlas
// window, from a shared frame bank. All calls run on the render thread.
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "SpikeC", __VA_ARGS__)

namespace {

std::vector<uint8_t> g_bank;  // frames × size × size × 4, premultiplied RGBA
int g_frames = 0;
int g_size = 0;

const uint8_t* BankFrame(int i) {
  return g_bank.data() + static_cast<size_t>(i % g_frames) * g_size * g_size * 4;
}

// Copies one sticker frame to (x, y) in a locked buffer. Bounds-checked.
void Blit(const ANativeWindow_Buffer& buf, int x, int y, const uint8_t* src) {
  if (x < 0 || y < 0 || x + g_size > buf.width || y + g_size > buf.height) return;
  auto* base = static_cast<uint8_t*>(buf.bits);
  for (int r = 0; r < g_size; ++r) {
    std::memcpy(base + (static_cast<size_t>(y + r) * buf.stride + x) * 4,
                src + static_cast<size_t>(r) * g_size * 4, g_size * 4);
  }
}

// Time spent in lock / copy / unlockAndPost since the last reset (ns).
// Watchdog breadcrumbs: phase (1 lock, 2 copy, 3 post, 0 idle) and window.
std::atomic<int> g_phase{0}, g_window{-1};
std::atomic<int64_t> g_lock_ns{0}, g_copy_ns{0}, g_post_ns{0}, g_calls{0};

int64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// Distinct buffer addresses seen per window: how many buffers each queue
// actually cycles through (the emulator reports no graphics memory).
std::mutex g_seen_mu;
std::map<ANativeWindow*, std::set<void*>> g_seen;

int Lock(ANativeWindow* win, ANativeWindow_Buffer* buf) {
  int rc = ANativeWindow_lock(win, buf, nullptr);
  if (rc != 0) {
    LOG("ANativeWindow_lock failed: %d", rc);
    return rc;
  }
  std::lock_guard<std::mutex> l(g_seen_mu);
  g_seen[win].insert(buf->bits);
  return 0;
}

// Atlas cell of `pitch` px with the sticker inset by `gutter` px. The gutter
// is cleared to transparent on every write, because each queued buffer holds
// stale contents and bilinear sampling reads across slot edges.
void BlitCell(const ANativeWindow_Buffer& buf, int x, int y, int pitch,
              int gutter, const uint8_t* src) {
  if (x < 0 || y < 0 || x + pitch > buf.width || y + pitch > buf.height) return;
  if (gutter == 0) {
    Blit(buf, x, y, src);
    return;
  }
  auto* base = static_cast<uint8_t*>(buf.bits);
  for (int r = 0; r < pitch; ++r) {
    uint8_t* row = base + (static_cast<size_t>(y + r) * buf.stride + x) * 4;
    const int sr = r - gutter;
    if (sr < 0 || sr >= g_size) {
      std::memset(row, 0, pitch * 4);
      continue;
    }
    std::memset(row, 0, gutter * 4);
    std::memcpy(row + gutter * 4, src + static_cast<size_t>(sr) * g_size * 4,
                g_size * 4);
    std::memset(row + (gutter + g_size) * 4, 0, (pitch - gutter - g_size) * 4);
  }
}

// Phase offset so neighbouring stickers show different frames.
int FrameFor(int sticker, int tick) { return tick + sticker * 7; }

}  // namespace

extern "C" {

JNIEXPORT void JNICALL Java_dev_webmtexture_spike_1c_SpikeNative_setBank(
    JNIEnv* env, jclass, jbyteArray bank, jint frames, jint size) {
  const jsize len = env->GetArrayLength(bank);
  if (frames <= 0 || size <= 0 || len != frames * size * size * 4) {
    LOG("setBank: bad size %d (%d frames of %d)", len, frames, size);
    return;
  }
  g_bank.resize(len);
  env->GetByteArrayRegion(bank, 0, len, reinterpret_cast<jbyte*>(g_bank.data()));
  g_frames = frames;
  g_size = size;
}

JNIEXPORT jlong JNICALL Java_dev_webmtexture_spike_1c_SpikeNative_attach(
    JNIEnv* env, jclass, jobject surface, jint w, jint h) {
  ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
  if (win == nullptr) {
    LOG("ANativeWindow_fromSurface returned null");
    return 0;
  }
  int rc = ANativeWindow_setBuffersGeometry(win, w, h, WINDOW_FORMAT_RGBA_8888);
  if (rc != 0) LOG("setBuffersGeometry(%d, %d) = %d", w, h, rc);
  return reinterpret_cast<jlong>(win);
}

JNIEXPORT void JNICALL Java_dev_webmtexture_spike_1c_SpikeNative_release(
    JNIEnv*, jclass, jlong handle) {
  if (handle != 0) ANativeWindow_release(reinterpret_cast<ANativeWindow*>(handle));
}

// Standalone mode: one lock/blit/post per window. Returns the number of
// failed windows.
JNIEXPORT jint JNICALL Java_dev_webmtexture_spike_1c_SpikeNative_tickTextures(
    JNIEnv* env, jclass, jlongArray windows, jint tick) {
  if (g_frames == 0) return -1;
  const jsize n = env->GetArrayLength(windows);
  std::vector<jlong> wins(n);
  env->GetLongArrayRegion(windows, 0, n, wins.data());
  int failed = 0;
  for (int i = 0; i < n; ++i) {
    auto* win = reinterpret_cast<ANativeWindow*>(wins[i]);
    if (win == nullptr) continue;
    ANativeWindow_Buffer buf;
    const int64_t t0 = NowNs();
    g_window = i;
    g_phase = 1;
    if (Lock(win, &buf) != 0) {
      ++failed;
      continue;
    }
    const int64_t t1 = NowNs();
    g_phase = 2;
    Blit(buf, 0, 0, BankFrame(FrameFor(i, tick)));
    const int64_t t2 = NowNs();
    g_phase = 3;
    if (ANativeWindow_unlockAndPost(win) != 0) ++failed;
    g_phase = 0;
    const int64_t t3 = NowNs();
    g_lock_ns += t1 - t0;
    g_copy_ns += t2 - t1;
    g_post_ns += t3 - t2;
    ++g_calls;
  }
  return failed;
}

// Atlas mode: one lock, `count` blits into row-major slots, one post. The
// whole buffer is locked as dirty, so the producer never copies back the
// previous buffer; unused atlas area is left undefined (never sampled).
JNIEXPORT jint JNICALL Java_dev_webmtexture_spike_1c_SpikeNative_tickAtlas(
    JNIEnv*, jclass, jlong handle, jint count, jint tick, jint gutter) {
  auto* win = reinterpret_cast<ANativeWindow*>(handle);
  if (win == nullptr || g_frames == 0) return -1;
  ANativeWindow_Buffer buf;
  const int64_t t0 = NowNs();
  g_window = 0;
  g_phase = 1;
  if (Lock(win, &buf) != 0) return 1;
  const int64_t t1 = NowNs();
  g_phase = 2;
  const int pitch = g_size + 2 * gutter;
  const int per_row = buf.width / pitch;
  for (int i = 0; i < count; ++i) {
    BlitCell(buf, i % per_row * pitch, i / per_row * pitch, pitch, gutter,
             BankFrame(FrameFor(i, tick)));
  }
  const int64_t t2 = NowNs();
  g_phase = 3;
  const int rc = ANativeWindow_unlockAndPost(win) != 0 ? 1 : 0;
  g_phase = 0;
  const int64_t t3 = NowNs();
  g_lock_ns += t1 - t0;
  g_copy_ns += t2 - t1;
  g_post_ns += t3 - t2;
  ++g_calls;
  return rc;
}

JNIEXPORT jint JNICALL Java_dev_webmtexture_spike_1c_SpikeNative_progress(
    JNIEnv*, jclass) {
  return g_phase.load() * 1000 + g_window.load();
}

// {lock, copy, post} total ns and the number of lock/post pairs; resets.
JNIEXPORT jlongArray JNICALL Java_dev_webmtexture_spike_1c_SpikeNative_phaseStats(
    JNIEnv* env, jclass, jboolean reset) {
  jlong out[4] = {g_lock_ns.load(), g_copy_ns.load(), g_post_ns.load(),
                  g_calls.load()};
  if (reset) g_lock_ns = g_copy_ns = g_post_ns = g_calls = 0;
  jlongArray arr = env->NewLongArray(4);
  env->SetLongArrayRegion(arr, 0, 4, out);
  return arr;
}

// {total distinct buffers, max per window, windows}.
JNIEXPORT jintArray JNICALL Java_dev_webmtexture_spike_1c_SpikeNative_bufferStats(
    JNIEnv* env, jclass) {
  jint out[3] = {0, 0, 0};
  {
    std::lock_guard<std::mutex> l(g_seen_mu);
    for (const auto& [win, bufs] : g_seen) {
      out[0] += static_cast<jint>(bufs.size());
      if (static_cast<jint>(bufs.size()) > out[1]) out[1] = bufs.size();
      ++out[2];
    }
  }
  jintArray arr = env->NewIntArray(3);
  env->SetIntArrayRegion(arr, 0, 3, out);
  return arr;
}

}  // extern "C"
