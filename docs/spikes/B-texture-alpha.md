# Spike B: texture transparency

- **Task:** T0.8 🛑 · **Status:** done. **Decision:** the Dart `ui.Image` fallback (T4.4) is not needed ([ADR 0005](../decisions/0005-no-ui-image-fallback.md))
- **Date:** 2026-11-09 · **Flutter:** 3.47.9 (`.fvmrc`)
- **Code:** [`example/spike_b/`](../../example/spike_b/) (throwaway). Reproduce with the commands in [Reproducing](#reproducing).
- **Screenshots:** [`B-texture-alpha/`](B-texture-alpha/). They are cropped to the tiles and status text but keep their original geometry, so `tool/sample.py` can re-sample them. Every sample result is in [`results.jsonl`](B-texture-alpha/results.jsonl).

> [!WARNING]
> **Emulators and the simulator only.** The task asks for 3+ physical Android devices (one low-end) and a physical iPhone. The project owner asked for this run to use emulators on both platforms instead, so every row below is an emulator or simulator. The emulator GPUs are software or translated: Vulkan is **lavapipe** on every AVD, GLES is **SwiftShader** (`swangle`) on API 37 and the host GPU on API 29/24. That means no vendor driver (Adreno, Mali, PowerVR, Apple GPU on device) was exercised. The owner accepted this evidence for the gate decision ([ADR 0005](../decisions/0005-no-ui-image-fallback.md)). Real-device rows are optional follow-up (see [Optional follow-up on real hardware](#optional-follow-up-on-real-hardware)).

## Devices

| ID | Kind | OS | Screen | GPU path |
|---|---|---|---|---|
| `api37` | Android emulator `Medium_Phone_API_37.0` (google_apis_playstore, arm64) | Android 17 (API 37) | 1080×2400, 420 dpi | gfxstream, Vulkan = lavapipe, GLES = swangle |
| `api29` | Android emulator `SpikeB_API29` (default, arm64, pixel_5 profile) | Android 10 (API 29) | 1080×2340 | gfxstream, Vulkan = lavapipe, GLES = host |
| `api24` | Android emulator `SpikeB_API24` (default, arm64, pixel_5 profile) | Android 7.0 (API 24) | 1080×2340 | gfxstream, GLES = host |
| `ios` | iOS Simulator, iPhone 17e | iOS 27.2 | 1170×2532 @3x | Impeller Metal (simulator) |

Host: Apple M4 Max, macOS 27.2, Android emulator 37.2.13. No iOS 15 simulator runtime can be installed here, so the owner chose to skip iOS 15 and only iOS 27.2 was tested.

## Summary

1. **Alpha composites correctly on every production path.** Every combination that a real app can reach was compared against Flutter's own canvas drawing the same pattern over the same background. The renderers were Impeller Vulkan, Impeller OpenGLES and Skia GLES on Android, and Impeller Metal on iOS. The producers were `ImageReader` (API 29+), `SurfaceTexture` (API < 29) and `CVPixelBuffer` on iOS. All 120 valid blocks per screenshot matched within **±1** (max error 0–1 of 255) over all four backgrounds.
2. **Every path treats texture contents as premultiplied** (`out = src + dst·(1−a)`). The invalid-premultiplied probes prove it: `(255,255,255,0)` adds to the background, and `(255,0,0,128)` matches the premultiplied formula with 0 error versus up to 255 for the straight-alpha reading. No path re-multiplied, un-premultiplied or colour-converted the data. That confirms the plan as written: the core writes premultiplied RGBA (Android) or BGRA (iOS), and fully transparent pixels must be exactly `(0,0,0,0)`.
3. **Skia + `ImageReader` + `SurfaceLifecycle.manual` loses the texture on background.** After a background/foreground cycle, a texture that receives no new frame renders **opaque black** (not transparent), and no callback fires. This reproduced on API 37 (twice) and API 29. Two things fix it: redrawing on resume, or using `resetInBackground`, which delivers `onSurfaceCleanup`/`onSurfaceAvailable`. Impeller (Vulkan and GLES) keeps the image with `manual`.
4. **Impeller Vulkan + `SurfaceTexture` producer shows nothing.** The texture stays fully transparent, frames never appear, and nothing is logged. Normal apps only reach this path through the debug flag `debugForceSurfaceProducerGlTextures`. The engine *also* picks `SurfaceTexture` for Huawei devices on API ≤ 29 (`hasAndroidHardwareBufferDefect`), and Impeller can choose Vulkan from API 29. A Huawei API 29 phone with Vulkan is therefore a plausible real-world hit. **Not verified: no such device was available.**
5. **iOS:** `textureFrameAvailable` from a background serial queue behaves exactly like calling it on main (no warnings, same results). `copyPixelBuffer` is always called off the main thread, and Flutter calls it again after foregrounding. The last buffer must therefore stay alive and be returned again. Content survives backgrounding.
6. **No case in this run needs the `ui.Image` fallback.** Where something broke, the cause was surface lifecycle (finding 3) or an unsupported producer/backend pairing (finding 4), never alpha handling. The owner decided T4.4 is not needed (see [Decision](#decision)).

## Method

The test pattern is 256×128 px of 32-px uniform blocks (8 × 4), premultiplied RGBA, generated in Dart ([`main.dart`](../../example/spike_b/lib/main.dart)):

| Row | Blocks |
|---|---|
| 0 | white, alpha 0, 36, 73, 109, 146, 182, 219, 255 |
| 1 | red, same alpha steps |
| 2 | azure `(0,128,255)`, same alpha steps |
| 3 | clear `(0,0,0,0)`, opaque red/green/blue, half grey `(128,128,128,128)`, half green `(0,128,0,128)`, probe `(255,255,255,0)`, probe `(255,0,0,128)` |

The two probes are not valid premultiplied colours (colour > alpha). They exist only to tell a premultiplied compositor apart from a straight-alpha one.

A second, animated texture (128×128) has a frame counter in binary in its top 16 px over red with cycling alpha. It shows that frames keep coming and that a texture survives backgrounding while it is being redrawn.

Each run lays out four rows (checkerboard with 16-px cells, magenta→cyan→yellow gradient, black, white) of three tiles each:
1. the texture over the background;
2. the same pattern drawn with Flutter's `Canvas` (straight colour = premultiplied·255/a) over the background;
3. the background alone.

All geometry is in physical pixels, and textures are drawn 1:1 with `FilterQuality.none`.

[`tool/sample.py`](../../example/spike_b/tool/sample.py) samples each block at 4 points (the centres of the checker cells inside it) in all three tiles. A block **passes** when every channel of texture vs Flutter is within ±3. It also computes the expected value under premultiplied (`s + C·(1−a)`) and straight (`s·a + C·(1−a)`) blending from the control tile, and labels each partial-alpha or probe block with the closer one. A run **passes** at 120/120 valid blocks (30 valid blocks × 4 backgrounds).

Each configuration is screenshotted twice: 6 s after launch, then again after going to the home screen for 5 s and coming back.

## Correctness matrix

"Backend" is the one the engine logged (`Using the Impeller rendering backend (…)`; Skia logs the opt-out warning). "Max err" is the largest channel difference from the Flutter-drawn tile, across all valid blocks and backgrounds.

### Android

| Device | Backend | Producer | Lifecycle | Launch | After background | Screenshots |
|---|---|---|---|---|---|---|
| api37 | Impeller **auto → OpenGLES** | ImageReader | manual | ✅ 120/120, max 1 | ✅ 120/120, max 1 | [launch](B-texture-alpha/api37_auto.launch.png) · [resume](B-texture-alpha/api37_auto.resume.png) |
| api37 | Impeller Vulkan | ImageReader | manual | ✅ 120/120, max 0 | ✅ 120/120, max 0 | [launch](B-texture-alpha/api37_vulkan.launch.png) · [resume](B-texture-alpha/api37_vulkan.resume.png) |
| api37 | Skia GLES | ImageReader | manual | ✅ 120/120, max 0 | ❌ **static texture opaque black** | [launch](B-texture-alpha/api37_skia.launch.png) · [resume](B-texture-alpha/api37_skia.resume.png) |
| api37 | Skia GLES | ImageReader | manual + redraw on resume | ✅ 120/120, max 0 | ✅ 120/120, max 0 | [launch](B-texture-alpha/api37_skia_redraw.launch.png) · [resume](B-texture-alpha/api37_skia_redraw.resume.png) |
| api37 | Impeller Vulkan | ImageReader | resetInBackground | ✅ 120/120, max 0 | ✅ 120/120, max 0 | [launch](B-texture-alpha/api37_vulkan_reset.launch.png) · [resume](B-texture-alpha/api37_vulkan_reset.resume.png) |
| api37 | Impeller OpenGLES | ImageReader | resetInBackground | ✅ 120/120, max 1 | ✅ 120/120, max 1 | [launch](B-texture-alpha/api37_gles_reset.launch.png) · [resume](B-texture-alpha/api37_gles_reset.resume.png) |
| api37 | Skia GLES | ImageReader | resetInBackground | ✅ 120/120, max 0 | ✅ 120/120, max 0 | [launch](B-texture-alpha/api37_skia_reset.launch.png) · [resume](B-texture-alpha/api37_skia_reset.resume.png) |
| api37 | Impeller OpenGLES | SurfaceTexture (forced) | manual | ✅ 120/120, max 1 | ✅ 120/120, max 1 | [launch](B-texture-alpha/api37_gles_st.launch.png) · [resume](B-texture-alpha/api37_gles_st.resume.png) |
| api37 | Skia GLES | SurfaceTexture (forced) | manual | ✅ 120/120, max 0 | ✅ 120/120, max 0 | [launch](B-texture-alpha/api37_skia_st.launch.png) · [resume](B-texture-alpha/api37_skia_st.resume.png) |
| api37 | Impeller Vulkan | SurfaceTexture (forced) | manual | ❌ **nothing rendered** | ❌ nothing rendered | [launch](B-texture-alpha/api37_vulkan_st.launch.png) · [resume](B-texture-alpha/api37_vulkan_st.resume.png) |
| api29 | Impeller **auto → OpenGLES** | ImageReader | manual | ✅ 120/120, max 1 | ✅ 120/120, max 1 | [launch](B-texture-alpha/api29_auto.launch.png) · [resume](B-texture-alpha/api29_auto.resume.png) |
| api29 | Impeller Vulkan | ImageReader | manual | ✅ 120/120, max 0 | ✅ 120/120, max 0 | [launch](B-texture-alpha/api29_vulkan.launch.png) · [resume](B-texture-alpha/api29_vulkan.resume.png) |
| api29 | Skia GLES | ImageReader | manual | ✅ 120/120, max 0 | ❌ **static texture opaque black** | [launch](B-texture-alpha/api29_skia.launch.png) · [resume](B-texture-alpha/api29_skia.resume.png) |
| api29 | Skia GLES | ImageReader | resetInBackground | ✅ 120/120, max 0 | ✅ 120/120, max 0 | [launch](B-texture-alpha/api29_skia_reset.launch.png) · [resume](B-texture-alpha/api29_skia_reset.resume.png) |
| api29 | Impeller auto → OpenGLES | ImageReader | resetInBackground | ✅ 120/120, max 1 | ✅ 120/120, max 1 | [launch](B-texture-alpha/api29_auto_reset.launch.png) · [resume](B-texture-alpha/api29_auto_reset.resume.png) |
| api24 | **Skia GLES** (Impeller needs API 29+) | SurfaceTexture (only option < API 29) | manual | ✅ 120/120, max 0 | ✅ 120/120, max 0 | [launch](B-texture-alpha/api24_default.launch.png) · [resume](B-texture-alpha/api24_default.resume.png) |
| api24 | Skia GLES | SurfaceTexture | resetInBackground (no effect) | ✅ 120/120, max 0 | ✅ 120/120, max 0 | [launch](B-texture-alpha/api24_reset.launch.png) · [resume](B-texture-alpha/api24_reset.resume.png) |

The two Skia `manual` failures turn 116 of 120 blocks wrong. The 4 that "pass" are the clear block on the black background, which is correct by accident. `api37_skia` was run twice with the same result (`api37_skia_rerun` and `api37_auto_rerun` are kept out of the committed screenshots).

On the API 37 and API 29 emulators, auto-selection picked **OpenGLES over Vulkan**, most likely because Impeller rejects the lavapipe software Vulkan driver (not confirmed). Forcing Vulkan through the manifest worked, so both Impeller backends are covered. On real devices, auto-selection will usually pick Vulkan on API 29+.

### iOS

| Device | Backend | Producer | `textureFrameAvailable` from | Launch | After background | Screenshots |
|---|---|---|---|---|---|---|
| ios (simulator) | Impeller Metal | `CVPixelBuffer` BGRA, IOSurface, Metal-compatible, from a `CVPixelBufferPool` | background serial queue | ✅ 120/120, max 1 | ✅ 120/120, max 1 | [launch](B-texture-alpha/ios_bg.launch.png) · [resume](B-texture-alpha/ios_bg.resume.png) |
| ios (simulator) | Impeller Metal | same | main queue | ✅ 120/120, max 1 | ✅ 120/120, max 1 | [launch](B-texture-alpha/ios_main.launch.png) · [resume](B-texture-alpha/ios_main.resume.png) |

`FLTEnableWideGamut` was left at its default. Even so, texture and canvas colours agree to ±1, so the BGRA buffer is treated as sRGB like the rest of the frame.

## Premultiply behaviour

- **Every path composites the texture as premultiplied source-over.** For each of the 86 partial-alpha or probe blocks per screenshot, the premultiplied prediction matched (error 0–1). The straight-alpha prediction was off by up to 255.
- **The probes confirm it directly.** `(255,255,255,0)` brightens the background additively: white on black, and checker/gradient pushed toward white. `(255,0,0,128)` over black gives `(255,0,0)` exactly, where straight alpha would give `(128,0,0)`.
- **What the core must do:**
  1. Output **premultiplied** pixels. That is libyuv `I420AlphaToABGR/ARGB(..., attenuate=1)`, which spike A already validated.
  2. Never emit colour > alpha. Any rounding or scaling step that can do so must clamp, or it shows as an additive halo at edges.
  3. Clear fully transparent pixels to exactly 0.

  GIF/WebP paths (T5.x) that produce straight alpha must premultiply before writing.
- **No byte-order surprises.** `ANativeWindow` RGBA_8888 (`R,G,B,A` in memory) and iOS `32BGRA` (`B,G,R,A` in memory) both displayed the intended colours. That matches `AGENTS.md`: libyuv `ABGR` on Android, `ARGB` on iOS.

## Surface lifecycle observations

**Android, `ImageReader` producer (API 29+):**
- `manual` + Impeller (Vulkan or GLES): no callbacks on background/foreground, and the last image is kept.
- `manual` + Skia: no callbacks, and the image **is lost**. The texture draws **opaque black** until a new frame is posted. An animated texture recovers on its next frame, but a paused or static one stays black.
- `resetInBackground`: `onSurfaceCleanup` on background and `onSurfaceAvailable` on foreground, under all three renderers. Re-acquiring `getSurface()` → `ANativeWindow_fromSurface` → redraw in `onSurfaceAvailable` gives a correct image.
- `onSurfaceCleanup` runs on the platform thread. The spike released the `ANativeWindow` synchronously on its render thread (posting, then waiting on a latch) before returning. Across all 19 Android runs: no `ANativeWindow_lock`/`unlockAndPost` failures, no unexpected buffer formats, and `setBuffersGeometry(RGBA_8888)` succeeded on the `PRIVATE`-format ImageReader surface.

**Android, `SurfaceTexture` producer (API < 29, or forced):**
- No callbacks under either lifecycle (`resetInBackground` is ignored on this path), and the image survived backgrounding under Skia and Impeller GLES.
- Under Impeller Vulkan, nothing displays at all (see finding 4).

**Engine detach:** a `createSurfaceProducer` call that arrived while the engine was detaching threw `RuntimeException: Cannot execute operation because FlutterJNI is not attached to native`. The spike hit this once when the app was relaunched mid-start. The plugin must catch it and return a `PlatformException`.

**Producer calls:** the spike called `producer.scheduleFrame()` after every post. Whether ImageReader frames show up without it was not tested.

**iOS:**
- No surface is lost on background. On resume, Flutter called `copyPixelBuffer` again (static texture: 1 draw, 2 copies), so the last `CVPixelBuffer` must stay referenced and be returned again.
- All `copyPixelBuffer` calls were off the main thread (`copiesOffMain == copies`).
- Each frame went into a fresh buffer from the pool, so the buffer Flutter samples is never written. 468 draws produced 449 copies in one run: Flutter coalesces notifications, which is fine.
- The simulator suspends the app in the background, so the animation timer stopped too.

## Implications for later tasks

- **T2.2 (Android glue):** create producers with `SurfaceLifecycle.resetInBackground`, and redraw the current frame from the cache in `onSurfaceAvailable`. This is correct on all three renderers and frees buffers in the background. If `manual` is kept for some reason, the current frame of every live animation **must** be redrawn on resume, or Skia devices (API < 29 and Vivante GPUs, which fall back from Impeller) show black boxes. Also catch the detached-engine exception, and consider logging the producer class plus the backend line (or failing loudly) for the Vulkan + `SurfaceTexture` combination.
- **T2.3 (iOS glue):** call `textureFrameAvailable` from the worker that produced the frame. A hop to main is not needed. Keep the latest buffer in a lock-protected slot, return a +1 reference from `copyPixelBuffer`, and write each frame into a pool buffer, never the published one.
- **T2.5 / T3.x:** the spike's Android animation kept drawing while the app was backgrounded (with `manual`). The scheduler should pause on `paused`/`hidden` anyway.
- **T6.x golden tests:** `tool/sample.py`'s texture-vs-canvas comparison is a cheap, tolerance-based alpha check that needs no reference images. It could be reused in `integration_test`.

## Optional follow-up on real hardware

The gate is closed without these, but anyone with devices can run `tool/run_android.sh` and `tool/run_ios.sh` (see the [README](../../example/spike_b/README.md)) and add rows here for:
1. Physical Android phones, especially a low-end one (e.g. Mali or PowerVR, Android 9–11) and a Huawei on API 29 (finding 4). Use auto backend, Vulkan, OpenGLES and Skia with `manual` and `reset`.
2. A physical iPhone.

An iOS 15 simulator is not an option: no runtime can be installed alongside Xcode 27.

## Decision

**Decided by the owner at the gate: the Dart `ui.Image` atlas fallback (T4.4) is not needed** ([ADR 0005](../decisions/0005-no-ui-image-fallback.md)).

Every production renderer and producer composites premultiplied alpha correctly. The failures were lifecycle and pairing issues that the native glue handles, so the rules under [Implications for later tasks](#implications-for-later-tasks) moved into T2.2 and T2.3. The owner accepted the emulator-only evidence and chose to skip iOS 15.

## Reproducing

```sh
cd example/spike_b && fvm flutter pub get
# Android (one emulator or device attached; AD = the agent-device command from T3's device panel)
tool/run_android.sh api37_auto   out
tool/run_android.sh api37_vulkan out spikeBackend=vulkan
tool/run_android.sh api37_skia   out spikeImpeller=false
tool/run_android.sh api37_skia_redraw  out spikeImpeller=false -- REDRAW_ON_RESUME=true
tool/run_android.sh api37_vulkan_reset out spikeBackend=vulkan -- LIFECYCLE=reset
tool/run_android.sh api37_gles_reset   out spikeBackend=opengles -- LIFECYCLE=reset
tool/run_android.sh api37_skia_reset   out spikeImpeller=false -- LIFECYCLE=reset
tool/run_android.sh api37_gles_st      out spikeBackend=opengles -- FORCE_GL=true
tool/run_android.sh api37_skia_st      out spikeImpeller=false -- FORCE_GL=true
tool/run_android.sh api37_vulkan_st    out spikeBackend=vulkan -- FORCE_GL=true
# (same set with api29_/api24_ labels on the other AVDs)
# iOS Simulator
UDID=<udid> tool/run_ios.sh ios_bg   out
UDID=<udid> tool/run_ios.sh ios_main out IOS_NOTIFY=main
# Re-sample a committed screenshot
python3 tool/sample.py ../../docs/spikes/B-texture-alpha/api37_skia.resume.png \
  ../../docs/spikes/B-texture-alpha/layout-android-1080x2400.json
```
