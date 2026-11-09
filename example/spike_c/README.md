# Spike C: grid density (T0.9)

A throwaway app, separate from the plugin. It animates N synthetic 128-px stickers at 30 fps in one of two ways:

- `MODE=textures`: one platform texture per sticker. On Android that is a `SurfaceProducer`, on iOS a `FlutterTexture`.
- `MODE=atlas`: one atlas texture (default 2048²). Each sticker is a `ClipRect` + `Transform.translate` + `Texture(atlasId)` sized to the whole atlas, which is the shape T4.3 plans for `StickerAtlasScope` children.

The report is [`docs/spikes/C-density.md`](../../docs/spikes/C-density.md).

Native code owns one render thread, as the plugin's scheduler will. Every 1/30 s it copies the next frame of each sticker from a 30-frame bank (generated in Dart) into the platform buffer(s), then notifies Flutter. Both modes do the same copy work, so the difference between them comes from the number of textures, buffer queues and notifications.

- **Android** ([`MainActivity.kt`](android/app/src/main/kotlin/dev/webmtexture/spike_c/MainActivity.kt), [`spike_c.cc`](android/app/src/main/cpp/spike_c.cc)): `SurfaceProducer`s with `resetInBackground` (ADR 0005), filled through `ANativeWindow_lock`/`unlockAndPost`. One main-thread hop per tick calls `scheduleFrame()` on every producer.
- **iOS** ([`AppDelegate.swift`](ios/Runner/AppDelegate.swift)): `FlutterTexture`s backed by one `CVPixelBufferPool` each (BGRA, IOSurface). `textureFrameAvailable` is called from the render queue.

After `WARMUP_SECS` the app records the following for `RUN_SECS`, then logs one `SPIKE_C_RESULT` JSON (in chunks):
- Flutter `FrameTiming`s, the same data as the DevTools frame chart: build (UI thread) and raster durations.
- Native tick timings.
- Process CPU time.
- Memory: RSS/PSS on Android, `phys_footprint` on iOS.
- The number of distinct buffers each queue cycled through. The emulator reports 0 KB of graphics memory, so this count is the way to size buffer memory.

## Switches (`--dart-define`)

| Switch | Default | Effect |
|---|---|---|
| `MODE` | `textures` | `textures` or `atlas` |
| `COUNT` | `60` | Number of stickers. `0` gives an idle baseline: the same app with no textures. |
| `ATLAS_W`, `ATLAS_H` | `2048` | Atlas size in px |
| `GUTTER` | `0` | Atlas only: a transparent border of this many px around each slot |
| `RUN_SECS`, `WARMUP_SECS` | `120`, `10` | Measurement window |
| `LABEL` | | Copied into the result |

Gradle `-P spikeImpeller=false` / `-P spikeBackend=vulkan|opengles` select the Android renderer, as in spike B.

## Running

```sh
cd example/spike_c && fvm flutter pub get
# One configuration (profile APK; `flutter run --profile` refuses emulators)
AD=<agent-device> tool/run_android.sh api37_auto_tex out -- MODE=textures
AD=<agent-device> tool/run_android.sh api37_vk_atlas out spikeBackend=vulkan -- MODE=atlas
# The whole matrix for one emulator (full | lowend | old)
AD=<agent-device> tool/matrix_android.sh api37 out full
# iOS Simulator (debug only)
AD=<agent-device> UDID=<udid> tool/run_ios.sh ios_tex out MODE=textures
# Tables for the report
python3 tool/table.py out/results.jsonl
```

Each run appends one JSON line to `out/results.jsonl`, keeps a screenshot taken mid-run, and saves `dumpsys meminfo` (Android) or `footprint` (iOS) output, plus host `top` samples (%CPU and energy impact) of the emulator or simulator-app process.
