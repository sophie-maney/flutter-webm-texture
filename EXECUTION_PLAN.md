# Execution Plan

These are the step-by-step tasks for agents. [`PLAN.md`](PLAN.md) explains *why*; this file says *what to do next*. Read [`AGENTS.md`](AGENTS.md) first.

**How to use this file**
- Pick the first unticked task whose **Depends** are all ticked. Tasks in the same **Track** with satisfied dependencies can run in parallel.
- Each task is one branch and one PR. Tick its checkbox in that PR.
- 🛑 marks a human gate: write the report, open the PR, and stop.

## Model & effort guide

| Model | Use it for |
|---|---|
| **Opus 5.5** (`claude-opus-5-5`) | C/C++ codecs and parsers, concurrency, GPU/texture plumbing, public API design, security review, spikes with open-ended debugging |
| **Sonnet 5.5** (`claude-sonnet-5-5`) | Well-specified Dart/Kotlin/Swift, tests, CI, build scripts with a clear recipe, docs |
| **Haiku 5.5** (`claude-haiku-5-5`) | Mechanical work: license aggregation, boilerplate, renames, formatting fixes |

| Effort | When |
|---|---|
| `low` | Scaffolding, boilerplate, docs with a known outline |
| `medium` | Normal feature work with a clear spec and tests to satisfy |
| `high` | Native code, platform graphics, tricky lifecycles, anything where a subtle bug ships |
| `xhigh` | Concurrency, memory ownership across threads or languages, spikes that decide architecture |
| `max` | Final security review only (T6.1) |

If a task fails its acceptance checks twice, rerun it one model tier or one effort level higher instead of retrying blindly.

---

## Phase 0: Scaffold and de-risk

### [x] T0.1 Pin Flutter and scaffold the plugin
**Model** Sonnet 5.5 · `low` · **Depends** none · **Track** A
- `fvm install 3.47.9 && fvm use 3.47.9` (commits `.fvmrc`; add `.fvm/` to `.gitignore`).
- `fvm flutter create --template=plugin --platforms=android,ios -a kotlin -i swift --org <org> --project-name webm_texture .` Keep the existing `README.md` content, `PLAN.md`, `AGENTS.md` and `assets/`.
- Set Android `minSdk 24` and iOS deployment target 15.0 in the plugin and the example.
- Add `analysis_options.yaml` (flutter_lints), a `.clang-format` (Google style), and a `.gitignore` covering `build/` and `third_party/prebuilt/`.
- Create empty dirs with `.gitkeep`: `src/`, `native_test/`, `fuzz/`, `third_party/`, `tool/`, `docs/spikes/`, `docs/decisions/`, `test_assets/`.

**Accept:** `fvm flutter analyze` is clean · `fvm flutter test` passes · the example builds with `fvm flutter build apk --debug` and `fvm flutter build ios --simulator --debug`.

### [x] T0.2 CI skeleton
**Model** Sonnet 5.5 · `low` · **Depends** T0.1 · **Track** A
- `.github/workflows/ci.yml`: set up fvm from `.fvmrc`, then format check, analyze, `flutter test`, and example Android + iOS-simulator debug builds. Cache pub and gradle.

**Accept:** the workflow is green on the PR.

### [x] T0.3 Test corpus generator
**Model** Sonnet 5.5 · `medium` · **Depends** T0.1 · **Track** B
- `tool/gen_corpus.sh`: uses ffmpeg (`libvpx-vp9`/`libvpx`, `-pix_fmt yuva420p`) to generate our **own** clips into `test_assets/`, one per case:

  | Case | What it covers |
  |---|---|
  | `vp9_alpha_512.webm` | 3 s at 30 fps |
  | `vp9_opaque_256.webm` | No alpha track |
  | `vp8_alpha_256.webm` | VP8 with alpha |
  | `vp9_alpha_vfr.webm` | Variable frame durations |
  | `vp9_odd_301x199.webm` | Odd dimensions |
  | `vp9_single_frame.webm` | One frame only |
  | `vp9_with_audio.webm` | Has an audio track |
  | `vp9_2048.webm` | Over the size limit |
  | `truncated.webm` | Cut mid-cluster |
  | `garbage.bin` | Not a media file |
  | `anim.webp` | Animated WebP |
  | `anim_disposal.gif` | Covers every GIF disposal mode |

- Reference frames: `test_assets/ref/<name>/<frame>.rgba.gz` (gzipped raw premultiplied RGBA), decoded with `ffmpeg -c:v libvpx-vp9`, for a sampled set of frames listed in `manifest.json` (w, h, frame count, durations, `ref_frames`). See [ADR 0001](docs/decisions/0001-sampled-reference-frames.md).
- Keep the corpus under 5 MB and commit it. Use no third-party or Telegram stickers.

**Accept:** the script is idempotent · `manifest.json` validates against the files · the size limit holds.

### [x] T0.4 iOS prebuilt libraries
**Model** Opus 5.5 · `high` · **Depends** T0.1 · **Track** C
- `third_party/versions.lock`: pinned tags and tarball sha256 for libvpx, libwebm, libyuv, libwebp, giflib and lz4.
- `third_party/build/build-ios.sh`: device arm64 + simulator arm64/x86_64 → `WebmTextureDeps.xcframework` (static, all libs merged, headers included).
  - libvpx flags: `--disable-encoders --enable-vp8-decoder --enable-vp9-decoder --disable-vp9-highbitdepth --size-limit=1024x1024 --enable-pic --disable-examples --disable-tools --disable-docs --disable-unit-tests`.
- `third_party/build/collect-licenses.sh` writes `third_party/LICENSES.md`.

**Accept:** the script runs from a clean checkout on macOS · `lipo -info` shows the expected slices · the per-slice size is reported in the PR.

### [x] T0.5 Android prebuilt libraries
**Model** Opus 5.5 · `high` · **Depends** T0.1 · **Track** C (parallel with T0.4)
- `third_party/build/build-android.sh`: NDK r28+, API 24, with ABIs arm64-v8a, armeabi-v7a (NEON) and x86_64 (x86 optional, behind a flag). Produces static `.a` files plus headers. Link flags for the later `.so`: `-Wl,-z,max-page-size=16384`.

**Accept:** the script builds all ABIs in Docker or on macOS · `file`/`llvm-readelf` confirm the architectures · the per-ABI size is in the PR.

### [x] T0.6 Prebuilt CI + fetch tool
**Model** Sonnet 5.5 · `medium` · **Depends** T0.4, T0.5 · **Track** C
- `.github/workflows/prebuilts.yml`: runs on changes to `third_party/**` or on manual dispatch. Builds both platforms and uploads to a GitHub Release `deps-<hash>` with `SHA256SUMS`, where the hash covers `versions.lock` and `third_party/build/` ([ADR 0003](docs/decisions/0003-release-tag-covers-build-recipe.md)).
- `tool/fetch_prebuilts.dart`: reads `versions.lock`, downloads the matching release, verifies sha256 and unpacks into `third_party/prebuilt/{android,ios}`. It must fail closed on a mismatch.

**Accept:** a fresh clone followed by `fvm dart run tool/fetch_prebuilts.dart` populates `prebuilt/` · a tampered checksum fails.

### [x] T0.7 🛑 Spike A: VP9/VP8 alpha demux and decode
**Model** Opus 5.5 · `xhigh` · **Depends** T0.3, T0.4 or T0.5 (host build of the libs is fine) · **Track** B
- Write a throwaway host CLI in `spikes/alpha/`. It demuxes with libwebm `webm_parser`, decodes the colour stream and the `BlockAdditional` (ID 1) alpha stream with two vpx contexts, merges them to premultiplied RGBA, and compares against `test_assets/ref`.
- If `webm_parser` can't surface BlockAdditions cleanly, prototype a minimal EBML reader (EBML header, Segment, Tracks, Cluster, SimpleBlock, BlockGroup, BlockAdditions) and compare effort and robustness.
- Report in `docs/spikes/A-alpha.md`: the chosen demuxer, per-frame max abs error/PSNR, decode ms/frame on host, and pitfalls (alpha keyframes, `AlphaMode` flag, laced blocks).

**Accept:** the alpha of every corpus WebM matches the reference (PSNR ≥ 45 dB or exact). **Stop for a human decision on the demuxer.**

**Decided:** our own minimal EBML reader ([ADR 0004](docs/decisions/0004-own-webm-demuxer.md)); report in [`docs/spikes/A-alpha.md`](docs/spikes/A-alpha.md).

### [x] T0.8 🛑 Spike B: Texture transparency (needs a real device)
**Model** Opus 5.5 · `high` · **Depends** T0.1, T0.5, T0.4 · **Track** D
- In `example/spike_b/`, render a known premultiplied RGBA test pattern (gradient alpha, plus fully transparent and fully opaque areas) into:
  - **Android:** a `SurfaceProducer` via `ANativeWindow`. Test on Impeller Vulkan and on the OpenGLES fallback (`--no-enable-impeller` / manifest flag), on 3+ physical devices including one low-end.
  - **iOS:** `FlutterTexture` + IOSurface `CVPixelBuffer` BGRA, on a device and on the simulator.
- Place it over a checkerboard and a coloured gradient, then take screenshots and sample pixels.
- Report `docs/spikes/B-texture-alpha.md`: a correctness matrix (device × backend), premultiply behaviour, and surface-lifecycle observations (background/foreground).

**Accept:** the matrix is complete with screenshots. **Stop: a human decides whether the Dart `ui.Image` fallback (T4.4) is needed.**

**Decided:** the fallback is not needed ([ADR 0005](docs/decisions/0005-no-ui-image-fallback.md)). The owner accepted emulator/simulator evidence and skipped iOS 15. Report in [`docs/spikes/B-texture-alpha.md`](docs/spikes/B-texture-alpha.md).

### [x] T0.9 🛑 Spike C: Grid density, 60 textures vs one atlas (needs a real device)
**Model** Opus 5.5 · `high` · **Depends** T0.8 · **Track** D
- Animate 60 synthetic 128-px patterns at 30 fps two ways: (a) 60 `SurfaceProducer`/`FlutterTexture`s, (b) one 2048² texture with clipped `Texture` children.
- Measure with `fvm flutter run --profile` and the DevTools timeline: raster p90/p99, UI p90, RSS/graphics memory, and 2-minute battery/thermal drift. Use a low-end Android device and an iPhone 12 or older.
- Report in `docs/spikes/C-density.md`.

**Accept:** the numbers are in the report. **Stop: a human picks the default grid mode and updates `PLAN.md`.**

**Decided:** grids use atlas mode, with content-sized pages and a ≥ 2-px slot gutter; standalone stays the default outside a scope ([ADR 0006](docs/decisions/0006-atlas-for-grids.md)). The owner accepted emulator/simulator evidence. Report in [`docs/spikes/C-density.md`](docs/spikes/C-density.md).

---

## Phase 1: Native core (host-testable)

### [x] T1.1 Core skeleton and C ABI
**Model** Opus 5.5 · `high` · **Depends** T0.7 · **Track** A
- Root `CMakeLists.txt` with these options: `WT_HOST_TESTS`, `WT_SANITIZE` (ASan + UBSan), `WT_FUZZ`. It links the prebuilts, or a host build of the deps for macOS/Linux CI.
- `src/api/wt_api.h` defines:
  - `wt_status` codes
  - opaque handles `wt_source`, `wt_anim`
  - `wt_limits`, `wt_info`
  - pixel formats
  - ownership and threading rules in the header comments
- Add stub implementations returning `WT_ERR_UNIMPLEMENTED`, and GoogleTest wiring in `native_test/`.
- Add a host-native CI job (macOS + Linux) with sanitizers.

**Accept:** host tests build and run green under ASan/UBSan in CI · the header is reviewed against `PLAN.md`'s API.

### [x] T1.2 Byte sources and format sniffing
**Model** Sonnet 5.5 · `medium` · **Depends** T1.1 · **Track** A
- `src/demux/source.*`: memory-backed and file-backed (mmap) readers with bounds-checked reads.
- `sniff()` returns WebM, WebP, GIF or unknown, based on magic bytes.

**Accept:** unit tests cover every corpus file, plus empty/1-byte/garbage input.

### [x] T1.3 WebM demuxer
**Model** Opus 5.5 · `high` · **Depends** T1.2 · **Track** A
- Implement the minimal EBML reader chosen in spike A ([ADR 0004](docs/decisions/0004-own-webm-demuxer.md)) in `src/demux/`, starting from `spikes/alpha/demux_mini_ebml.cc` and carrying over the checks under "Pitfalls" in [`docs/spikes/A-alpha.md`](docs/spikes/A-alpha.md). Output: the video track (codec VP8/VP9, w/h, `AlphaMode`) and a frame index (offset, size, pts, duration, keyframe, alpha payload span). Detect audio tracks and ignore them.
- Durations come from block timestamps; when absent, fall back to `DefaultDuration`, then to 1/30 s.

**Accept:** index tests against `manifest.json` for every corpus WebM · `truncated.webm` produces a partial index plus a `WT_ERR_TRUNCATED` flag and no crash · a demux-only libFuzzer target runs 60 s clean (AGENTS.md: a new parser ships with a fuzz target).

### [x] T1.4 VPX decode, alpha merge and scaling
**Model** Opus 5.5 · `high` · **Depends** T1.3 · **Track** A
- `src/decode/vpx_decoder.*`: a colour context plus a lazily created alpha context. Output is YUVA420 at source size.
- `src/convert/`: libyuv scale to the size bucket (64/96/128/192/256/384/512), and conversion of YUVA420 to premultiplied RGBA or BGRA into a caller-provided buffer with stride.
- Add a minimal `wt_anim` that can `seek(0)` and `next_frame(dst, stride, fmt)`, synchronously and with no cache yet.

**Accept:** golden tests (T1.6) pass for VP8 and VP9 · an alpha-less file gives alpha = 255.

### [x] T1.5 Limits and probe
**Model** Sonnet 5.5 · `medium` · **Depends** T1.3 · **Track** B
- Enforce `wt_limits` (max dimension, frames, duration, bytes, total decoded pixels) at sniff, demux and decode time.
- `wt_probe()` fills `wt_info` with codec, w, h, fps, frame count, duration, has_alpha and has_audio.

**Accept:** `vp9_2048.webm` → `WT_ERR_LIMIT` · probe values match the manifest.

### [x] T1.6 Golden frame tests
**Model** Sonnet 5.5 · `medium` · **Depends** T1.4 · **Track** B
- Compare decoded frames at source size against `test_assets/ref` (PSNR ≥ 45 dB per frame), and spot-check scaled buckets with a looser tolerance.

**Accept:** green under sanitizers on macOS + Linux CI.

### [ ] T1.7 Fuzzing
**Model** Opus 5.5 · `high` · **Depends** T1.4 · **Track** C
- Add libFuzzer targets `fuzz_sniff`, `fuzz_webm` (demux + decode of the first N frames with tight limits) and `fuzz_probe`. Seed them from `test_assets/`.
- CI: a 60 s smoke run per target on PRs, and a nightly 30 min run that uploads crashes as artifacts.

**Accept:** 10 min locally per target with no findings, or the findings are fixed in the same PR with regression inputs added to the corpus.

### [ ] T1.8 Dart FFI bindings and `probe()`
**Model** Sonnet 5.5 · `medium` · **Depends** T1.5 · **Track** B
- `ffigen.yaml` over `wt_api.h` → `lib/src/gen/wt_bindings.g.dart`.
- `WebmStickers.probe(StickerSource)` runs on a background isolate (`Isolate.run`) and returns a `StickerInfo`.
- Wire the core into the Android CMake build and the iOS SwiftPM/podspec build, linked against the prebuilts.

**Accept:** an integration test in the example probes every corpus file on the Android emulator and the iOS simulator.

---

## Phase 2: Platform textures + standalone widget

### [ ] T2.1 Pigeon API
**Model** Sonnet 5.5 · `medium` · **Depends** T1.8 · **Track** A
- `pigeons/messages.dart` defines `createAnimation(sourceKey, bytesOrPath, bucketW, bucketH) → {animId, textureId}`, `setActive(animId, bool)`, `dispose(animId)`, `deviceSignals() → {ramMb, cores, lowRam, perfClass, lowPower, thermal}`, plus an error stream.

**Accept:** the generated code is committed, and analyze plus the native builds are clean.

### [ ] T2.2 Android texture glue
**Model** Opus 5.5 · `high` · **Depends** T2.1, T0.8 · **Track** B
- One `SurfaceProducer` per animation. Pass its `Surface` to native via JNI → `ANativeWindow_fromSurface`, set geometry to the bucket and RGBA_8888.
- A simple per-animation render thread (replaced in T3.3) calls `wt_anim_next_frame` into `ANativeWindow_lock`'d memory.
- Handle `SurfaceProducer.Callback` (`onSurfaceAvailable`/`onSurfaceCleanup`) by releasing and reacquiring the window. Leave no leaks on dispose.
- From spike B ([ADR 0005](docs/decisions/0005-no-ui-image-fallback.md)):
  - Create producers with `SurfaceLifecycle.resetInBackground`. With `manual`, Skia loses the image in the background and shows opaque black.
  - In `onSurfaceAvailable`, re-fetch `getSurface()` and redraw the current frame.
  - In `onSurfaceCleanup`, release the `ANativeWindow` synchronously before returning.
  - Catch the "FlutterJNI is not attached" exception from `createSurfaceProducer` and return an error.
  - Log the producer class at creation.

**Accept:** the example shows animated, transparent stickers on the API 24 and latest emulators · surviving 20 background/foreground cycles · `adb shell dumpsys meminfo` is stable after 100 create/dispose cycles.

### [ ] T2.3 iOS texture glue
**Model** Opus 5.5 · `high` · **Depends** T2.1, T0.8 · **Track** C (parallel with T2.2)
- An ObjC++ shim over `wt_api.h`. A Swift `FlutterTexture` uses a `CVPixelBufferPool` (BGRA, IOSurface, Metal-compatible) and native code writes into the locked buffer. Call `textureFrameAvailable` from the worker that produced the frame; spike B found no need to hop to main. Write each frame into a fresh pool buffer, keep the latest one referenced, and return it from every `copyPixelBuffer` call (Flutter asks again after foregrounding).
- Both `Package.swift` (binaryTarget/vendored xcframework plus a C++ target) and `.podspec` must build.

**Accept:** same as T2.2 on the latest iOS simulator (no iOS 15 runtime is available; see ADR 0005) · Instruments shows no leaks over 100 create/dispose cycles · SwiftPM and CocoaPods example builds are both green in CI.

### [ ] T2.4 Dart registry, dedupe and buckets
**Model** Sonnet 5.5 · `high` · **Depends** T2.1 · **Track** A
- `StickerRegistry` keys animations by (content hash, bucket). It refcounts acquire/release, disposes on zero after a short grace period (to avoid scroll thrash), and computes buckets from logical size × devicePixelRatio.

**Accept:** unit tests cover dedupe, refcount, grace-period dispose and bucket math, using a fake platform.

### [ ] T2.5 `WebmSticker` widget and visibility
**Model** Opus 5.5 · `high` · **Depends** T2.4, T2.2 or T2.3 · **Track** A
- The widget wraps a custom `RenderProxyBox` that computes viewport intersection during paint/layout. It combines that with `TickerMode` and app lifecycle to drive `setActive`.
- Props: `source`, `width`, `height`, `fit`, `autoplay`, `loop`, `controller`, `placeholder`, `errorBuilder`. The placeholder stays up until the first frame is ready.

**Accept:** widget tests cover visibility and TickerMode transitions (fake platform) · in a ListView of 200 items in the example, only on-screen items report active (verified via a debug overlay counter).

### [ ] T2.6 Sources and fetcher
**Model** Sonnet 5.5 · `medium` · **Depends** T2.4 · **Track** C
- `StickerSource.file/bytes/asset/network`. `StickerFetcher` is an abstract interface that returns a file path or bytes. Hash the content with SHA-1 of the bytes, or with a caller-provided `key`. Errors surface through `errorBuilder`.

**Accept:** unit tests for each source type with a fake fetcher.

### [ ] T2.7 Example app
**Model** Sonnet 5.5 · `low` · **Depends** T2.5 · **Track** C
- Pages: Chat (mixed text and stickers), Checkerboard (alpha correctness), Stress (200-item grid in standalone mode), Probe (file info).

**Accept:** runs on the emulator and the simulator · screenshots in the PR.

### [ ] T2.8 Integration tests
**Model** Sonnet 5.5 · `medium` · **Depends** T2.7 · **Track** C
- `example/integration_test/`: screenshot goldens of the checkerboard page per platform (with tolerance), plus playback-advances and pause-when-offscreen tests. Add them to CI (Android emulator, iOS simulator).

**Accept:** green in CI.

---

## Phase 3: Cache, scheduler, tiers

### [ ] T3.1 Frame cache format (`.wsc`)
**Model** Opus 5.5 · `high` · **Depends** T1.4 · **Track** A
- `docs/cache-format.md` specifies: magic, version, w/h, frame count, durations, offset table, LZ4 YUVA420 frames, and a CRC per frame.
- Writer: streams frames while the first playback runs, writes to a temp file then fsync and rename. Reader: mmap and bounds-checked.
- Integrate into `wt_anim`: if a cache exists, read from it; otherwise decode and write.

**Accept:** round-trip tests · corrupt/partial cache files are detected, deleted and re-decoded · fuzz target `fuzz_wsc`.

### [ ] T3.2 Disk budget and eviction
**Model** Sonnet 5.5 · `medium` · **Depends** T3.1 · **Track** A
- LRU by access time under `diskBudgetBytes`, plus `clearCache()` and `cacheStats()` exposed through FFI. Evict on startup and after each write.

**Accept:** unit tests for budget enforcement and concurrent access (two animations writing).

### [ ] T3.3 Scheduler
**Model** Opus 5.5 · `xhigh` · **Depends** T3.1, T2.2, T2.3 · **Track** B
- Replace the per-animation threads with one clock thread and N workers (N from the tier). Each animation has at most one frame in flight; late frames are dropped. The `maxConcurrent` limit is applied by priority (visible area, then recency), and the rest freeze on their current frame.
- The clock pauses entirely when nothing is active.

**Accept:** a TSan host stress test (500 animations churning) passes · 60 visible standalone stickers on the latest iOS simulator keep raster p90 ≤ 8 ms (spike C measured 5.9 ms; the Android emulator's gralloc is too slow for a standalone budget, see the report) · CPU drops to about 0 when everything is paused. Real-device budgets come from T3.5.

### [ ] T3.4 Device tiers
**Model** Sonnet 5.5 · `medium` · **Depends** T2.1, T3.3 · **Track** C
- Tier detection from `deviceSignals()` (thresholds in `PLAN.md`), with runtime downgrade on thermal state or Low Power Mode changes. Overrides come from `StickerConfig`.

**Accept:** unit tests for the tier matrix · a manual toggle of Low Power Mode on the simulator downgrades the tier.

### [ ] T3.5 Benchmarks (needs a real device)
**Model** Sonnet 5.5 · `medium` · **Depends** T3.3 · **Track** C
- `example/integration_test/perf_test.dart` with `traceAction` measures scrolling the stress grid. A summary JSON is uploaded as an artifact, plus a short `docs/perf.md` table per device.

**Accept:** the numbers are recorded for at least one low-end Android device and one older iPhone.

---

## Phase 4: Atlas mode (grids)
*Go ahead: grids use atlas mode ([ADR 0006](docs/decisions/0006-atlas-for-grids.md)).*

### [ ] T4.1 Slot allocator
**Model** Sonnet 5.5 · `high` · **Depends** T0.9 · **Track** A
- Pure C++ shelf packing per bucket in atlas pages up to 2048 px wide: alloc/free/compaction stats. Allocation fails when full so the caller can grow the page or add one.
- From spike C ([ADR 0006](docs/decisions/0006-atlas-for-grids.md)):
  - Page height is the rows in use, rounded up to 16 px, not a fixed 2048. Report the page size so callers can resize.
  - Each slot has a ≥ 2-px transparent gutter: the slot pitch is the bucket + 2 × gutter, still 16-px aligned.

**Accept:** unit tests plus a randomized alloc/free property test.

### [ ] T4.2 Native atlas compositor
**Model** Opus 5.5 · `xhigh` · **Depends** T4.1, T3.3 · **Track** A
- One platform texture per atlas page. Scheduler workers write frames into slot regions of the locked buffer and present once per tick per page. Animations can switch between standalone and atlas.
- From spike C:
  - Lock the whole buffer (`dirty = nullptr`) so the producer never copies back.
  - Rewrite every live slot, and clear its gutter, on every tick, because queued buffers hold stale frames.
  - Notify once per page per tick; on Android, one `scheduleFrame()` per page.
  - Report buffer memory per page as distinct buffers × page bytes.

**Accept:** a TSan stress test passes · the device demo shows 60+ animations with one texture per page.

### [ ] T4.3 `StickerAtlasScope` widget
**Model** Opus 5.5 · `high` · **Depends** T4.2 · **Track** A
- An inherited scope owns the atlas pages. Child `WebmSticker`s render a clipped and translated `Texture(atlasId)` for their slot, and slots are released when children deactivate.

**Accept:** the Stress page in atlas mode matches standalone output visually, with no slot bleed (golden) · perf meets the T0.9 targets: with 60 stickers, Flutter renders at the content rate (no extra frames) and every tick is delivered on the API 37 emulator and the latest iOS simulator, and iOS simulator raster p90 is ≤ 3 ms (spike C measured 2.0 ms).

### ~~T4.4 Dart `ui.Image` atlas fallback (only if T0.8 requires it)~~ (dropped)
**Dropped:** T0.8 found that platform textures composite alpha correctly ([ADR 0005](docs/decisions/0005-no-ui-image-fallback.md)).

**Model** Opus 5.5 · `high` · **Depends** T4.2 · **Track** B
- Native fills a shared RGBA buffer per page. Dart creates one `ui.Image` per page per tick via `decodeImageFromPixels` and draws slots with `canvas.drawImageRect`. Double-buffer it, and dispose old images.

**Accept:** correct alpha on the devices that failed in T0.8 · perf numbers are in `docs/perf.md`.

---

## Phase 5: Animated WebP + GIF

### [ ] T5.1 Animated WebP
**Model** Sonnet 5.5 · `medium` · **Depends** T3.1 · **Track** A
- `WebPAnimDecoder` produces an RGBA canvas, which is converted to the YUVA cache path. Respect per-frame durations and the limits.

**Accept:** golden tests against reference frames · fuzz target `fuzz_webp`.

### [ ] T5.2 GIF
**Model** Sonnet 5.5 · `high` · **Depends** T3.1 · **Track** B
- giflib with our own compositing (disposal none/background/previous, transparency index, local palettes). Clamp delays below 20 ms to 100 ms, as browsers do.

**Accept:** golden tests for `anim_disposal.gif` · fuzz target `fuzz_gif`.

---

## Phase 6: Hardening + release

### [ ] T6.1 Security review
**Model** Opus 5.5 · `max` · **Depends** all of Phases 1–5 · **Track** A
- Audit every parser and decoder path, `wt_api.h` ownership, the JNI/ObjC++ boundaries and the limits.
- Run each fuzz target for 4+ hours. Run host tests under ASan, UBSan and TSan.
- Write `docs/security.md` covering the threat model and findings.

**Accept:** no open high or critical findings.

### [ ] T6.2 Documentation
**Model** Sonnet 5.5 · `medium` · **Depends** T4.3 · **Track** B
- README (features, limits, quick start, atlas mode, fetcher example, tiers, cache), dartdoc for all public APIs, and `CHANGELOG.md`.

**Accept:** `fvm dart doc` produces no warnings.

### [ ] T6.3 Licenses and privacy manifest
**Model** Haiku 5.5 · `low` · **Depends** T0.4 · **Track** C
- Aggregate third-party licenses into `LICENSE` and NOTICE, and add `ios/.../PrivacyInfo.xcprivacy` (no tracking, no required-reason APIs unless used; file timestamps are used by the cache, so declare `C617.1`).

**Accept:** every lib in `versions.lock` has its license text included.

### [ ] T6.4 Publish pipeline
**Model** Sonnet 5.5 · `medium` · **Depends** T6.2, T6.3, T0.6 · **Track** B
- A release workflow: tag → fetch prebuilts → vendor them into the package (verifying size limits) → `pana` → `flutter pub publish --dry-run`. The real publish stays manual or uses OIDC.

**Accept:** pana score is at or near maximum · the dry run passes.

### [ ] T6.5 Upstream watch
**Model** Haiku 5.5 · `low` · **Depends** T0.6 · **Track** C
- A scheduled workflow checks upstream tags and OSV for libvpx, libwebp, giflib, libyuv and lz4, then opens an issue or PR to bump `versions.lock`.

**Accept:** a manual dispatch produces a correct report.

### [ ] T6.6 Final review
**Model** Opus 5.5 · `xhigh` · **Depends** T6.1–T6.5 · **Track** A
- Review the whole package for API consistency, dead code, threading docs and test gaps. File follow-up issues, and fix anything small inline.

**Accept:** 🛑 the human signs off on 1.0.0.
