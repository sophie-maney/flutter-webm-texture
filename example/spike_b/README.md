# Spike B: texture transparency (T0.8)

This is a throwaway app, separate from the plugin. It renders a known premultiplied RGBA test pattern into a platform texture and draws it over four backgrounds: a checkerboard, a colour gradient, black and white. Next to each texture tile it puts the same pattern drawn by Flutter's own canvas, and the bare background as a control. The report is [`docs/spikes/B-texture-alpha.md`](../../docs/spikes/B-texture-alpha.md).

- **Android** ([`MainActivity.kt`](android/app/src/main/kotlin/dev/webmtexture/spike_b/MainActivity.kt), [`spike_b.cc`](android/app/src/main/cpp/spike_b.cc)) uses a `SurfaceProducer` and fills it from C++ via JNI: `ANativeWindow_fromSurface` → `setBuffersGeometry(RGBA_8888)` → `ANativeWindow_lock` on a render thread.
- **iOS** ([`AppDelegate.swift`](ios/Runner/AppDelegate.swift)) uses a `FlutterTexture` backed by a `CVPixelBufferPool` of BGRA, IOSurface, Metal-compatible buffers.
- [`tool/sample.py`](tool/sample.py) (stdlib only) reads a screenshot and the `SPIKE_B_LAYOUT` log lines. It compares 4 points in each of the 32 blocks of every texture tile against the Flutter-drawn tile, and classifies the blending as premultiplied or straight.

## Switches

| Switch | Values | Effect |
|---|---|---|
| Gradle `-P spikeImpeller` | `true` (default), `false` | `EnableImpeller` manifest flag (`false` = Skia) |
| Gradle `-P spikeBackend` | `auto` (default), `vulkan`, `opengles` | `ImpellerBackend` manifest flag |
| `--dart-define=FORCE_GL` | `true` | Sets `FlutterRenderer.debugForceSurfaceProducerGlTextures`, which forces the `SurfaceTexture` producer on API 29+ |
| `--dart-define=LIFECYCLE` | `manual` (default), `reset` | `SurfaceLifecycle` passed to `createSurfaceProducer` |
| `--dart-define=REDRAW_ON_RESUME` | `true` | Redraws the static texture on `AppLifecycleState.resumed` |
| `--dart-define=IOS_NOTIFY` | `bg` (default), `main` | iOS: the thread that calls `textureFrameAvailable` |

## Running

The run scripts build one configuration, install it, take a screenshot at launch and another after 5 s in the background, then sample both. UI actions go through T3's `agent-device` command (`$AD`). `adb` and `simctl` handle only the install and the logs.

```sh
cd example/spike_b
fvm flutter pub get
AD=<agent-device> tool/run_android.sh api37_vulkan out spikeBackend=vulkan
AD=<agent-device> tool/run_android.sh api37_skia_st out spikeImpeller=false -- FORCE_GL=true
AD=<agent-device> UDID=<sim-udid> tool/run_ios.sh ios_bg out
# Re-sample a committed screenshot:
python3 tool/sample.py ../../docs/spikes/B-texture-alpha/api37_vulkan.launch.png \
  ../../docs/spikes/B-texture-alpha/layout-android-1080x2400.json
```

Run by hand (`fvm flutter run`), the app shows the active backend and producer class, the per-texture counters (draws, errors, surface callbacks, `copyPixelBuffer` calls) and the app lifecycle transitions.
