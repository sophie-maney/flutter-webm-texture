# 0011: Android texture glue lives in libwebm_texture.so

- **Status:** Accepted (T2.2)
- **Date:** 2026-11-10

## Context

T2.2 connects each Flutter `SurfaceProducer` to the core. [ADR 0009](0009-ffi-loading-and-native-wiring.md) built the core into `libwebm_texture.so` for `dart:ffi` and exported only `wt_*`. The glue needs `ANativeWindow`, which only native code can lock, so it needs a JNI layer and a place for its per-animation render thread. Building it settled four things.

1. A second `.so` for JNI would link a second copy of the core and the prebuilt codecs, next to the one Dart opens.
2. `ImageReaderSurfaceProducer` (API 29+) schedules an engine frame itself when an image arrives, and the `SurfaceTexture` path marks the texture frame available. Calling `scheduleFrame()` after every frame, as spike B did, isn't needed.
3. On API 37, ImageReader's native listener attached each render thread to the JVM and detached it again around every posted frame. That allocates a `java.lang.Thread` per frame per animation, and you can see it in the thread names, which cycle through `Thread-N`.
4. Without a frame cache (T3.1), redrawing a paused animation into a new surface costs either a second CPU copy of every frame or a re-decode.

## Decision

- The JNI glue (`android/src/main/cpp/jni_bridge.cc`, `surface_player.cc`) is compiled into the same `libwebm_texture.so` as the core. Kotlin loads it with `System.loadLibrary("webm_texture")`. Besides `wt_*`, the library now exports the `Java_com_example_webm_1texture_WtNative_*` entry points, and nothing else. The glue includes only `wt_api.h`.
- Each animation gets one native render thread (`SurfacePlayer`), which T3.3 will replace. The thread owns the `wt_anim` and the `ANativeWindow`, and it loops at `WT_END`. It is attached to the JVM once, as `wt-anim`, for its whole lifetime. Kotlin only creates producers, routes lifecycle callbacks and forwards errors.
- No `scheduleFrame()` calls.
- A redraw into a new surface while paused seeks back to the frame that was showing and decodes it again. While playing, the next frame is drawn instead. T3.1's cache will make the seek cheap.
- Frame-decode failures are reported once per animation on `streamErrors`, and playback continues, as `wt_api.h` intends. Lock failures are reported as `SURFACE_LOST`, at most once per failure episode.
- The plugin ships consumer R8 rules (`android/proguard-rules.pro`) for the JNI names and the error callback.

## Consequences

- One copy of the core per process, shared by Kotlin and Dart.
- `onSurfaceCleanup` and `dispose` block the main thread for at most one in-flight frame write, because the seek runs outside the window lock.
- A paused redraw after backgrounding can take a decode from the previous keyframe until T3.1 lands.
- On API < 29 (`SurfaceTexture`), no lifecycle callbacks arrive, so animations keep decoding in the background until Dart deactivates them (T2.5).
