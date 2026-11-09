# 0005: No Dart `ui.Image` fallback; platform textures everywhere

- **Status:** Accepted (T0.8, human decision at the Spike B gate)
- **Date:** 2026-11-09

## Context

`PLAN.md` kept a fallback in reserve in case Android platform textures couldn't composite alpha correctly: atlas mode would switch to a Dart-side `ui.Image` per page per tick (T4.4). Spike B ([report](../spikes/B-texture-alpha.md)) rendered a premultiplied test pattern into platform textures and compared it pixel by pixel with Flutter's own canvas over four backgrounds. The textures were a `SurfaceProducer` filled through `ANativeWindow` on Android, and a `FlutterTexture` with an IOSurface BGRA `CVPixelBuffer` on iOS.

- Every renderer and producer pairing a real app can reach composited correctly, within ±1 of 255. On Android that is Impeller Vulkan, Impeller OpenGLES and Skia, with the ImageReader (API 29+) or SurfaceTexture (API < 29) producer. On iOS it is Impeller Metal.
- Every path treats texture contents as premultiplied.
- The failures were not alpha problems:
  - Skia + ImageReader + `SurfaceLifecycle.manual` drops the image on background and shows opaque black.
  - Impeller Vulkan + a SurfaceTexture producer renders nothing.

The spike ran on Android emulators (API 37, 29 and 24) and the iOS Simulator (iOS 27.2) only, at the owner's request. The task had asked for physical devices. No iOS 15 simulator runtime can be installed alongside Xcode 27.

## Decision

- T4.4 is dropped. Standalone and atlas modes both use platform textures on Android and iOS.
- The owner accepts the emulator/simulator evidence for this decision and skips iOS 15 simulator testing.
- The lifecycle and threading rules from the spike become requirements of T2.2 and T2.3:
  - **Android:** create producers with `SurfaceLifecycle.resetInBackground`. Redraw the current frame in `onSurfaceAvailable`. Release the `ANativeWindow` synchronously in `onSurfaceCleanup`. Catch the detached-engine exception from `createSurfaceProducer`.
  - **iOS:** call `textureFrameAvailable` from the worker that produced the frame. Keep the latest buffer referenced and return it from `copyPixelBuffer` on every call. Write each frame into a fresh pool buffer.

## Consequences

- One rendering path per platform, and no per-tick `ui.Image` allocation or `decodeImageFromPixels` copy.
- Vendor driver bugs on real devices (Adreno, Mali, PowerVR) were not exercised. If one shows up, the fix starts at the native glue. A Dart fallback would need a new ADR.
- A Huawei API ≤ 29 device running Impeller Vulkan would pair Vulkan with the SurfaceTexture producer, which rendered nothing on the emulator. The glue should log the producer class at creation so that this case is diagnosable.
