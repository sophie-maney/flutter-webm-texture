# Spike C: grid density, 60 textures vs one atlas

- **Task:** T0.9 🛑 · **Status:** measured, **waiting for the owner's decision** (see [Decision](#decision)).
- **Date:** 2026-11-09 · **Flutter:** 3.47.9 (`.fvmrc`)
- **Code:** [`example/spike_c/`](../../example/spike_c/) (throwaway). Reproduce with the commands in [Reproducing](#reproducing).
- **Data:** every run's result is one line in [`C-density/results.jsonl`](C-density/results.jsonl). The tables below come from `tool/table.py` on that file.

> [!WARNING]
> **Emulators and the simulator only.** The task asks for a low-end Android phone and an iPhone 12 or older. As with spike B, the owner asked for emulators and the simulator instead. That limits the evidence in three ways:
> - **No battery or thermal data.** Emulator battery and thermal state are fixed values, and the simulator reports the Mac's thermal state (always nominal here). In their place the report gives process CPU time and the host's `top` %CPU and energy impact. Neither is a battery measurement.
> - **Android buffer costs are emulator costs.** `ANativeWindow_lock` goes through the emulator's gralloc (goldfish on API 29, gfxstream on API 37). Locking a 64 KB buffer costs 0.6–2.4 ms of CPU there, which is likely one to two orders of magnitude slower than on a phone. The textures-mode numbers on Android are therefore much worse than a phone would show, while the direction (per-buffer overhead × 60) holds.
> - **The iOS Simulator runs debug builds only.** It has no AOT, so Dart is JIT. Raster work runs on the Mac's GPU through Metal. iOS raster times are real engine work but on much faster hardware than an iPhone 12.
>
> The **ratios** between the two modes on the same device are the useful result. The absolute numbers are not a budget for real phones.

## Devices

| ID | Kind | OS | CPU / RAM | Renderer |
|---|---|---|---|---|
| `api37` | Android emulator `Medium_Phone_API_37.0`, started with `-cores 4` | Android 17 (API 37) | 4 vCPU / 2 GB | Impeller auto → OpenGLES (as in spike B), plus forced Vulkan (lavapipe) and Skia |
| `api29lo` | Android emulator `SpikeB_API29`, started with `-cores 2 -memory 2048` as a **low-end proxy** | Android 10 (API 29) | 2 vCPU / 2 GB | Impeller auto → OpenGLES |
| `ios` | iOS Simulator, iPhone 17e | iOS 27.2 | host (16 cores) | Impeller Metal |

Host: Apple M4 Max, macOS 27.2. The API 24 (Skia + `SurfaceTexture`) set was started, then stopped at the owner's request to save time. Only its idle baseline completed, and that run is not reported.

## Summary

1. **The atlas is cheaper on every device and backend tested.** With 60 stickers at 30 fps:
   - **iOS:** raster p90 is **2.0 ms** for the atlas and **5.9 ms** for textures. App CPU is 16% against 30% of one core. The native tick takes 2.1 ms against 8.0 ms.
   - **Android API 37:** raster p90 is **1.2 ms** for the atlas and **18.5 ms** for textures. The bigger difference is upstream: textures mode cannot even *produce* 30 fps. Each tick of 60 lock/copy/post cycles took 157 ms (p50), so only 643 of 3,600 ticks ran, about 5 fps per sticker. The atlas ran all 3,600 ticks in 4.6 ms (p50).
   - **Android low-end proxy (2 vCPU):** neither mode keeps up, but the atlas still gets **15.7 Flutter fps against 10.9**, with raster p50 of 59 ms against 89 ms.
2. **The gap grows with N.** At 120 stickers, iOS textures raster p90 goes from 5.9 to 9.9 ms while the atlas goes from 2.0 to 2.2 ms. On API 37, textures got 421/3,600 ticks against 3,600/3,600 for the atlas. The cost of textures mode scales with the number of textures, and the atlas cost barely moves.
3. **The atlas costs graphics memory, set by its page size and not by the sticker count.**
   - Android's `ImageReaderSurfaceProducer` allows up to 7 images per producer (`MAX_IMAGES = 7` in the embedding), and the spike saw 5–7 distinct buffers per queue. A 2048² page is 16 MB, so **80–112 MB per page**.
   - iOS cycled 2 pool buffers, so **32 MB**.
   - Sixty 128-px textures use 9–22 MB in total.
   - A **2048×512** page (64 slots of 128 px) gives the same raster and CPU results as 2048² at **one quarter of the memory**: 20–24 MB on Android, 8 MB on iOS. Atlas pages must therefore be sized to what they hold, not fixed at 2048².
4. **The atlas needs a gutter.** When slots are packed edge to edge, bilinear sampling at any non-1:1 scale pulls in the neighbouring slot's edge pixels. This showed as faint lines along every tile's bottom edge on both platforms. A 2-px transparent border around each slot, cleared on every write, removes it ([before](C-density/bleed_api37_atlas_g0.png) / [after](C-density/bleed_api37_atlas_g2.png) on Android, [before](C-density/bleed_ios_atlas_g0.png) / [after](C-density/bleed_ios_atlas_g2.png) on iOS). The gutter did not change the cost measurably.
5. **No drift over 2 minutes.** In every run, raster p90 and CPU per 30-s window stayed within run-to-run noise. On emulators and the simulator this mostly shows that the harness is stable. Real thermal throttling can't appear here.

## Method

- **Workload.** N stickers of 128×128 px, premultiplied, animating at 30 fps. Each is a soft-edged disc with a translucent halo orbiting the centre, with a binary frame counter in the top 8 rows. The 30 frames are generated once in Dart (RGBA on Android, BGRA on iOS). Every 1/30 s, one native render thread copies the next frame of every sticker into its platform buffer(s) and notifies Flutter. Both modes therefore do the **same copy work**, and decode cost is deliberately excluded. What differs is the number of textures, buffer queues and notifications.
- **Textures mode.** One `SurfaceProducer` (`resetInBackground`, per ADR 0005) or one `FlutterTexture` + `CVPixelBufferPool` per sticker. Each widget is `Texture(id)`.
- **Atlas mode.** One producer/texture of `ATLAS_W × ATLAS_H` (default 2048²). Slots are row-major at a 128-px pitch, or 128 + 2·gutter. Each sticker widget is `ClipRect → OverflowBox → Transform.translate → SizedBox(atlas size × scale) → Texture(atlasId)`, the child shape T4.3 plans. Each tick does one lock or pool buffer, N blits and one post or `textureFrameAvailable`.
- **Layout.** All stickers on one screen, 6 columns (≈180 px tiles on Android, 195 px on iOS), so textures are drawn scaled about 1.4× with the default filter.
- **Notification.** On Android, one main-thread hop per tick calls `scheduleFrame()` on every producer. That runs a full engine frame (Dart build + raster). On iOS, `textureFrameAvailable` is called from the render queue. That re-rasterises the last layer tree without a Dart frame.
- **Measurements.** After a 10-s warm-up, each run measured for **120 s**:
  - **Raster and UI.** Android: Flutter `FrameTiming`s (`addTimingsCallback`), which are the same data as the DevTools frame chart. iOS: texture-only frames report no `FrameTiming`, so `tool/timeline.dart` records the engine timeline through the VM service for 90 s (the data DevTools reads) and uses `Rasterizer::DrawToSurfaces`. iOS has no UI-thread number because no Dart frames run.
  - **Native tick:** wall time of each tick on the render thread, plus Android's per-phase split (lock / copy / post).
  - **CPU:** process utime + stime (`/proc/self/stat`, `getrusage`).
  - **Memory:** RSS and PSS on Android (`Debug.MemoryInfo`, `dumpsys meminfo`), `phys_footprint` on iOS (`task_info`, `footprint`). The emulator reports **0 KB of graphics memory**, and simulator IOSurfaces don't show in the app's footprint. Buffer memory is therefore given as **distinct buffers seen × buffer size**, counted from the addresses returned by `ANativeWindow_lock` and the IOSurface IDs from the pool.
  - **Host:** `top` %CPU and POWER (energy impact) of the emulator's `qemu` process or the simulator app process, every 5 s.
- **Builds.** Android uses profile APKs (`flutter run --profile` refuses emulators, so the APK is built and installed with adb). iOS uses a simulator debug build, with Runner's Swift compiled with `-O` so the copy loops aren't `-Onone`.

## Results

**Flutter fps** is how many frames Flutter rasterised per second; † marks the iOS timeline count. **Ticks** is native updates completed out of the 3,600 scheduled (30 fps × 120 s); fewer means stickers animated slower than 30 fps. **Buffer memory** is distinct buffers seen × buffer size.

### 60 stickers

| Device · backend | Mode | Flutter fps | Raster p50 / p90 / p99 (ms) | UI build p90 (ms) | Native tick p50 / p90 (ms) | Ticks | Buffer memory | App CPU (% of 1 core) | PSS / footprint (MB) |
|---|---|---|---|---|---|---|---|---|---|
| api37 · Impeller GLES | textures | 56.3 | 14.8 / 18.5 / 29.5 | 0.08 | 157 / 316 | **643** | 243 × 64 KB = 15 MB | 144 | 112 |
| api37 · Impeller GLES | **atlas 2048²** | 30.0 | 0.8 / 1.2 / 12.0 | 0.08 | 4.6 / 7.3 | 3,600 | 5 × 16 MB = **80 MB** | 20 | 107 |
| api37 · Impeller GLES | **atlas 2048×512** | 30.0 | 0.7 / 11.1 / 12.0 | 0.07 | 0.6 / 2.3 | 3,600 | 5 × 4 MB = **20 MB** | 11 | 108 |
| api37 · Impeller Vulkan | textures | 53.5 | 9.9 / 20.4 / 28.6 | 0.10 | 180 / 299 | **627** | 245 × 64 KB = 15 MB | 164 | 128 |
| api37 · Impeller Vulkan | **atlas 2048²** | 30.0 | 1.2 / 2.1 / 4.5 | 0.08 | 1.3 / 11.8 | 3,600 | 80 MB | 31 | 123 |
| api37 · Skia | textures | 56.8 | 10.8 / 17.4 / 28.1 | 0.07 | 140 / 241 | **783** | 268 × 64 KB = 17 MB | 126 | 112 |
| api37 · Skia | **atlas 2048²** | 30.0 | 0.6 / 10.5 / 11.5 | 0.08 | 1.1 / 3.8 | 3,600 | 80 MB | 12 | 108 |
| api29lo · Impeller GLES | textures | 10.9 | 88.7 / 116.5 / 165.2 | 5.3 | 68 / 86 | **1,246** | 360 × 64 KB = 22 MB | 154 | 112 |
| api29lo · Impeller GLES | **atlas 2048²** | 15.7 | 59.0 / 76.2 / 168.3 | 4.2 | 21 / 29 | 2,729 | 6 × 16 MB = **96 MB** | 127 | 179 |
| api29lo · Impeller GLES | **atlas 2048×512** | 15.6 | 57.0 / 79.4 / 188.7 | 3.7 | 13 / 19 | 2,831 | 6 × 4 MB = **24 MB** | 108 | 112 |
| ios · Impeller Metal | textures | 29.9† | 5.6 / 5.9 / 6.3 | n/a | 8.0 / 8.4 | 3,600 | 143 × 64 KB = 9 MB | 30 | 126 |
| ios · Impeller Metal | **atlas 2048²** | 29.9† | 1.8 / 2.0 / 2.2 | n/a | 2.1 / 2.3 | 3,600 | 2 × 16 MB = **32 MB** | 16 | 129 |
| ios · Impeller Metal | **atlas 2048×512** | 29.9† | 1.4 / 1.9 / 2.3 | n/a | 1.9 / 2.2 | 3,600 | 2 × 4 MB = **8 MB** | 15 | 128 |

Idle baselines (same app, no stickers): api37 0.4% CPU and 95 MB PSS, api29lo 1.2% and 80 MB, iOS 2.8% and 121 MB footprint.

### 120 stickers

| Device | Mode | Flutter fps | Raster p50 / p90 / p99 (ms) | Native tick p50 (ms) | Ticks | Buffer memory | App CPU (%) |
|---|---|---|---|---|---|---|---|
| api37 | textures | 53.2 | 17.6 / 26.2 / 28.7 | 269 | **421** | 480 × 64 KB = 30 MB | 142 |
| api37 | atlas 2048² | 30.0 | 1.1 / 1.8 / 12.2 | 1.3 | 3,600 | 80 MB | 12 |
| api29lo | textures | 5.4 | 178 / 224 / 274 | 145 | **676** | 720 × 64 KB = 45 MB | 169 |
| api29lo | atlas 2048² | 9.2 | 108 / 132 / 164 | 22 | 2,492 | 7 × 16 MB = 112 MB | 133 |
| ios | textures | 29.9† | 9.8 / 9.9 / 10.2 | 12.3 | 3,600 | 246 × 64 KB = 15 MB | 47 |
| ios | atlas 2048² | 29.9† | 1.6 / 2.2 / 2.5 | 3.2 | 3,600 | 32 MB | 19 |

### Where the Android time goes

The time is split per lock/post pair (one pair per window in textures mode, one per tick in atlas mode), and the render thread's CPU time over the 120-s window is shown alongside:

| Run | Lock (ms) | Copy (ms) | Post (ms) | Render-thread CPU |
|---|---|---|---|---|
| api37 textures (×60 per tick) | 2.44 | 0.002 | 0.40 | 104 s of 120 s |
| api37 atlas 2048² (×1) | 4.07 | 0.25 | 0.44 | 16 s |
| api37 atlas 2048×512 (×1) | 0.62 | 0.15 | 0.31 | 4 s |
| api29lo textures (×60) | 0.59 | 0.015 | 0.58 | 67 s |
| api29lo atlas 2048² (×1) | 11.6 | 1.8 | 8.6 | 44 s |
| api29lo atlas 2048×512 (×1) | 6.7 | 1.3 | 5.7 | 29 s |

- In textures mode the cost is per buffer: lock + post is about 1.2–2.8 ms × 60 windows per tick. The render thread is on CPU for most of the run, so this is gralloc work rather than waiting on the consumer.
- In atlas mode, lock and post scale with **page size**, which is another reason to size pages to their content.
- Copying the pixels is negligible in both modes.

On a phone, `ANativeWindow_lock` on an in-process `ImageReader` queue should cost tens of µs rather than ms. The textures-mode tick will be far shorter there, but it remains 60 lock/post pairs and 60 `ImageReader` acquisitions per frame against one.

### Raster-side observations

- **Android textures mode raises the Flutter frame rate.** Each producer's frames arrive at its own time and each one schedules an engine frame, so Flutter rendered **53–57 frames/s** to show 30-fps content (with only ~5 fps of real updates per sticker). Each of those frames imports up to 60 new images. That doubles the raster work again on top of the per-texture cost, and 1,500–4,300 frames per run went over 16 ms. The atlas rendered exactly 30 frames/s.
- **The Android atlas raster time is bimodal.** About 0.8 ms usually, with a cluster at 10–12 ms that sometimes reaches the p90 (Skia, the 2048×512 run, some 30-s windows). It shows up in every atlas run regardless of page size, so it is probably the emulator's image import or fence wait. It should be re-checked on a phone before T3.3 uses it as a budget.
- **iOS cost scales with the texture count.** `CompositorContext::ScopedFrame::Raster` is 5.7 ms for 60 textures against 0.8 ms for one atlas, and 9.8 ms for 120 textures. Encode time is similar in both modes. The extra cost is per-texture CPU work on the raster thread (`copyPixelBuffer` + Metal texture wrap per texture per frame), so it should carry over to devices in proportion.

### Drift (raster p90 ms / app CPU % per 30-s window)

| Run | 0–30 s | 30–60 s | 60–90 s | 90–120 s |
|---|---|---|---|---|
| api37 textures | 18.5 / 146 | 18.5 / 146 | 18.9 / 141 | 18.2 / 145 |
| api37 atlas 2048² | 1.3 / 20 | 0.9 / 18 | 1.3 / 20 | 10.7 / 20 |
| api29lo textures | 109 / 150 | 115 / 155 | 121 / 156 | 118 / 157 |
| api29lo atlas 2048² | 80 / 124 | 72 / 127 | 77 / 127 | 75 / 127 |
| ios textures | – / 30 | – / 30 | – / 30 | – / 30 |
| ios atlas 2048² | – / 16 | – / 16 | – / 16 | – / 16 |

There is no upward trend in any run. The api37 atlas jump in the last window is the 10–12 ms cluster described above, not a trend. Every run's window data is in `results.jsonl`.

### Host energy (battery stand-in)

Mean `top` %CPU and POWER of the emulator process (Android) or the simulator app (iOS) during the window:

| Device | Baseline | Textures | Atlas 2048² | Atlas 2048×512 |
|---|---|---|---|---|
| api37 (qemu) | 23 | 1,179 | 632 | 622 |
| api29lo (qemu) | 11 | 171 | 155 | 144 |
| ios (Runner) | 3 | 30 | 16 | 15 |

On the emulator this includes GPU emulation done on the host CPU, so it says little about a phone's battery beyond the direction.

### Screenshots

The modes look the same apart from gutter bleed: [api37 textures](C-density/api37_auto_tex.png) · [api37 atlas](C-density/api37_auto_atlas.png) · [iOS textures](C-density/ios_tex.png) · [iOS atlas](C-density/ios_atlas.png).

## Implications for later tasks

These hold whichever default the owner picks:

- **T4.1 (slot allocator):**
  - Pages are **not fixed at 2048²**. Allocate the page height (or both dimensions) for the slots needed, rounded to 16 px, and grow by adding rows or pages. Buffer memory is pages × page size × queue depth (5–7 on Android, 2–3 on iOS).
  - Every slot needs a **≥ 2-px transparent gutter**, so the pitch is 128 + 4 for 128-px slots. Write the gutter as zero on every frame (or once per buffer), because queued buffers come back with stale contents.
- **T4.2 (atlas compositor):**
  - Lock the whole buffer (`dirty = nullptr`) so the producer never copies back. Write every live slot each tick, because each queued buffer holds old frames.
  - Notify once per page per tick. On Android, call `scheduleFrame()` once per page.
- **T2.2 / T3.3 (standalone mode):**
  - Per-texture overhead is the main cost, so standalone mode should stay for small counts (inline chat stickers).
  - On Android, batch the `scheduleFrame()` hop: one main-thread post per tick for all producers, as the spike did. Each producer's frame still triggers its own engine frame, so standalone stickers that tick together should post together.
  - Under contention the spike's fixed-rate tick dropped late ticks rather than bunching them up. That is the right behaviour for the scheduler too.
- **T3.3's raster budget ("number from T0.9")** can't honestly come from these emulator numbers. A placeholder that matches what was measured would be "60 visible stickers in atlas mode: raster p90 ≤ 4 ms on the simulator, frame rate equal to the content rate", re-baselined on a phone in T3.5.

## Optional follow-up on real hardware

The comparison above is consistent, but every absolute number comes from an emulator or the simulator. Anyone with devices can run `tool/matrix_android.sh <prefix> out lowend` and `tool/matrix_ios.sh` (see the [README](../../example/spike_c/README.md)) to add rows for:
1. A low-end Android phone (Mali or PowerVR, Android 9–11). This shows whether textures mode's lock/post cost and the atlas's 10–12 ms raster cluster are emulator artefacts.
2. An iPhone 12 or older, for raster p90 and `phys_footprint` with real IOSurface accounting.
3. Battery and thermal drift: a 10-minute run per mode with the screen at fixed brightness, recording battery % and `thermalState`.

## Decision

**Pending: a human picks the default grid mode and updates `PLAN.md`.** This spike makes no decision.

For the owner:
- **What the data supports:** making atlas mode (`StickerAtlasScope`) the **default for grids**, with **content-sized pages** (not a fixed 2048²) and a **2-px gutter**, and keeping standalone textures for small inline counts.
- **The trade-off:** the atlas spends graphics memory per page instead of per sticker. A 2048² page costs 32 MB (iOS) to 80–112 MB (Android emulator queue depth), and a 2048×512 page costs a quarter of that.
- **The open question:** where the crossover lies. The spike only measured 60 and 120 stickers, so it can't say at what count (likely somewhere below 30) textures mode is the better default.

## Reproducing

```sh
cd example/spike_c && fvm flutter pub get
# Android (one emulator attached; AD = the agent-device command from T3's device panel)
emulator -avd Medium_Phone_API_37.0 -cores 4 &      # api37
AD=<agent-device> tool/matrix_android.sh api37 out full
emulator -avd SpikeB_API29 -cores 2 -memory 2048 &  # low-end proxy
AD=<agent-device> tool/matrix_android.sh api29lo out lowend
# iOS Simulator
AD=<agent-device> UDID=<udid> tool/matrix_ios.sh ios out
# Tables
python3 tool/table.py out/results.jsonl
```

The committed `results.jsonl` holds the final run of each configuration. Superseded runs are not kept: the first `api37_auto_*` runs, which had no per-phase timing, and two runs hit by a spike startup race that was later fixed in `Grid.start()`.
