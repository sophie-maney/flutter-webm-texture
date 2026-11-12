# webm_texture — Plan

> Step-by-step tasks for agents: [`EXECUTION_PLAN.md`](EXECUTION_PLAN.md). Agent rules: [`AGENTS.md`](AGENTS.md).

A Flutter plugin (Android + iOS) that plays animated stickers inline the way Telegram does: transparent, looping, cheap enough for a grid of 60. It decodes in software through a shared C++ core, caches frames once, and renders them to Flutter `Texture`s.

## Decisions

| Topic | Decision |
|---|---|
| Flutter | `fvm` pinned to **3.47.9** (latest stable as of 2026-11-09), committed in `.fvmrc` |
| Scope | **Playback only.** No encoding. `probe()` exposes metadata, and safety limits reject hostile files. |
| Formats | VP9 WebM (with alpha), VP8 WebM (with alpha), animated WebP, GIF |
| Density | Inline chat stickers (1–10 on screen) **and** picker grids (30–60+ on screen) |
| Min OS | Android API 24, iOS 15 |
| Sources | `file`, `bytes`, `asset` built in; `network` goes through a pluggable `StickerFetcher`. Content is keyed by SHA-1 of the bytes, a caller `key`, or a file path ([ADR 0013](docs/decisions/0013-sources-and-fetcher.md)) |
| Native libs | Built by CI per ABI with pinned versions, then vendored into the published package with sha256 checks |
| Low-end devices | Automatic device tiers; the app can override them |
| Distribution | Public on pub.dev eventually, so the API, docs, licenses and binary hosting must meet that bar from day one |

## Why libvpx (and not FFmpeg)

| | **libvpx + libyuv** + our own WebM demuxer (chosen) | Minimal FFmpeg |
|---|---|---|
| VP9/VP8 **alpha** | Decode the color stream and the alpha stream (Matroska `BlockAdditions`, `BlockAddID=1`) with two `vpx_codec_ctx_t`, then merge. Full control. | FFmpeg's built-in `vp9` decoder **drops alpha**. It only works through `libvpx-vp9`, so libvpx is needed anyway. |
| Size per ABI | ~0.8–1.2 MB (decoders only, `--disable-vp9-highbitdepth`) | 2–4 MB, even when stripped down |
| License | All BSD/MIT. No relinking obligations. | LGPL: needs dynamic linking or relinkable objects; awkward on iOS |
| Build | `configure` with a per-target triple, NEON intrinsics | Larger `configure` matrix, plus libvpx anyway |
| Other formats | Add libwebp (anim) + giflib (BSD/MIT, small) | One library for everything, but much heavier |

Platform decoders are ruled out. Android `MediaCodec` VP9 hardware paths discard the alpha side-stream and vary by vendor. iOS has no public WebM/VP9 decode path. The plugin uses software decoding everywhere, as Telegram does. Telegram's own clients are worth reading for reference: Android `AnimatedFileDrawable`/`BitmapsCache`; iOS `AnimationCache`/`MultiAnimationRenderer`.

**Third-party set:** libvpx (VP8/VP9 decoders), libyuv (scale, YUV→RGBA, premultiply via `I420AlphaToARGB/ABGR(..., attenuate=1)`), libwebp (`WebPAnimDecoder`), giflib, lz4. WebM demuxing is our own minimal EBML reader in `src/demux/` ([ADR 0004](docs/decisions/0004-own-webm-demuxer.md)), not libwebm. libwebm stays in `versions.lock` only until a follow-up PR removes it from the prebuilts.

## Architecture

```
Dart (lib/)                                   Native core (src/, C++17, shared Android+iOS)
─────────────────────────────────────         ─────────────────────────────────────────────
WebmSticker widget ──► StickerRegistry        Source sniff ─► Demuxer (WebM | AnimWebP | GIF)
   (TickerMode + viewport visibility)            │                 │
StickerAtlasScope (grid mode)                    │           Decoder (vpx color + vpx alpha | webp | gif)
WebmStickers.configure / probe                   │                 │
        │  Pigeon (lifecycle, textures)          │       libyuv: scale to target bucket → YUVA420
        │  dart:ffi (probe, cache admin)         │                 │
        ▼                                        ▼                 ▼
Platform glue                                 FrameCache (disk: LZ4 YUVA420 frames, LRU budget)
  Android: Kotlin + JNI                          │
    SurfaceProducer → ANativeWindow              ▼
  iOS: Swift + ObjC++ shim                    Scheduler: 1 clock thread + N workers (N = tier)
    FlutterTexture → CVPixelBuffer(IOSurface)    └─► YUVA→premultiplied RGBA/BGRA straight into
                                                     the platform buffer (ANativeWindow_lock / CVPixelBuffer)
```

### Core concepts
- **Animation** = (content hash, size bucket). Exactly one decoder/cache/texture exists per Animation, refcounted by every widget showing it. Identical stickers share a single `textureId`, and multiple `Texture` widgets can show the same id.
- **Size buckets:** the target size rounds up to buckets in physical pixels (e.g. 64/96/128/192/256/384/512). Decoding happens at display size, never at 512 when the widget is 80 px.
- **Two render modes, one widget:**
  1. **Standalone** (default): one platform texture per Animation.
  2. **Atlas** (`StickerAtlasScope`, **the mode for grids**, per [ADR 0006](docs/decisions/0006-atlas-for-grids.md)): one texture per atlas page. Pages are up to 2048 px wide and only as tall as the rows they need, rounded up to 16 px; a full 2048² page costs 32 MB (iOS) to 80–112 MB (Android) once the buffer queue is counted. Slots are 16-px aligned, with a ≥ 2-px transparent gutter against bilinear bleed. Native code blits each visible Animation into its slot. Each widget draws the atlas `Texture` clipped and translated to its slot. This avoids 60 platform textures and 60 buffer queues, and matches Telegram iOS's multi-animation renderer.
- **Scheduler:** a single clock, in the core, running at most at the tier's fps cap (60 by default; the display rate isn't read) and only while something is due. Each tick, it queues due frames to a small worker pool. Each Animation has at most one frame in flight, and late frames are dropped rather than queued: they are decoded but never shown, so playback keeps the content's timing. Over `maxConcurrent`, the stickers with the largest on-screen area play, then the most recently started; Dart reports the areas with `setPriority`. The glue gives the scheduler buffers through a `wt_sink` ([ADR 0017](docs/decisions/0017-scheduler.md)). A worker decodes each frame before it takes the sink's buffer, so the buffer (and on Android the window lock) is held only for the YUV→RGBA write; removing a player, moving it into or out of an atlas, or shrinking the pool cancels a frame in flight between frames of its catch-up instead of waiting for it ([ADR 0027](docs/decisions/0027-cancellable-frames.md)).
- **Frame cache:** the first playback decodes and writes `<hash>_<w>x<h>_v1.wsc` in the background. That file holds a header, per-frame durations, an offset table, and **LZ4-compressed YUVA420** frames (2.5 B/px before compression, versus 4 B/px for RGBA). The format is in [`docs/cache-format.md`](docs/cache-format.md); the caller names the file through `wt_anim_create_cached`, and the header fingerprints the source so a stale file is deleted rather than shown ([ADR 0015](docs/decisions/0015-frame-cache-file-and-abi.md)). Later loops and later sessions read only from the cache: LZ4 decompress plus a NEON YUV→RGBA pass per frame. The disk budget defaults to 200 MB with LRU eviction ([ADR 0016](docs/decisions/0016-cache-budget-and-eviction.md)). One file may take at most a quarter of the budget, and never more than 64 MiB: a write past that is abandoned, evicts nothing, and isn't tried again in the process, and the sticker decodes on every loop ([ADR 0026](docs/decisions/0026-cache-file-cap.md)). RAM holds only 2 frame buffers per active Animation, plus atlas memory.
- **Pausing:** a widget animates only when `TickerMode` is enabled **and** it intersects the viewport. Off-screen widgets inside `cacheExtent` keep their first frame ready. Everything pauses when the app is `hidden`, `paused` or `detached`; `inactive` keeps playing because the app is still on screen.
- **Tiers:** auto-detected from RAM, CPU cores, Android `isLowRamDevice`/media performance class, iOS `physicalMemory`, Low Power Mode, and thermal state (which can change at runtime).
  - high: source fps, unlimited animations
  - mid: ≤30 fps, ≤30 concurrent
  - low: ≤15 fps, ≤8 concurrent, the rest show a static first frame
  - Apps override this with `StickerConfig(tier:, maxFps:, maxConcurrent:)`. The rules (thresholds, how Low Power Mode and thermal state lower the tier, worker counts) are in [ADR 0018](docs/decisions/0018-device-tiers.md); `WebmStickers.debugTier` shows the tier in force.
- **Safety:** input from other users is untrusted.
  - libvpx is built with `--size-limit=1024x1024`, which only the VP9 decoder enforces; every VP8/VP9 keyframe must also have the track's size, checked before libvpx sees it ([ADR 0025](docs/decisions/0025-security-review-hardening.md)).
  - The core enforces limits (configurable): max dimensions, frame count, duration, file size, and total decode budget.
  - Malformed input becomes an error state in the widget, never a crash.
  - Demux/decode entry points are fuzzed.
  - The threat model, the review findings and how to report a vulnerability are in [`docs/security.md`](docs/security.md).

### Dart API (draft)
```dart
await WebmStickers.configure(StickerConfig(
  cacheDirectory: ..., diskBudgetBytes: 200 << 20,   // both implemented in T3.2
  tier: StickerTier.auto, maxFps: null, maxConcurrent: null,
  limits: StickerLimits(maxDimension: 1024, maxDuration: Duration(seconds: 10), maxBytes: 2 << 20),
  fetcher: myFetcher, // implements StickerFetcher for StickerSource.network (until `configure` lands: WebmStickers.fetcher = myFetcher, ADR 0013)
));

WebmSticker(
  source: StickerSource.file(path) | .bytes(data, key:) | .asset(name) | .network(url),
  width: 120, height: 120, fit: BoxFit.contain,
  autoplay: true, loop: true, controller: ctrl,          // play/pause/seekToFrame (ADR 0014)
  placeholder: ..., errorBuilder: (ctx, err) => ...,
);

StickerAtlasScope(child: GridView.builder(...));         // grid mode
final info = await WebmStickers.probe(source);           // codec, w, h, fps, frames, duration, hasAlpha, hasAudio
await WebmStickers.clearCache();
final stats = await WebmStickers.cacheStats();          // fileCount, totalBytes
WebmStickers.debugActiveCount;                           // ValueListenable<int>: widgets animating now
```

## Repository layout
```
.fvmrc                     # flutter 3.47.9
pubspec.yaml               # package: webm_texture (name TBC)
lib/                       # public API, registry, visibility, pigeon + ffigen bindings
pigeons/                   # Pigeon definitions
src/                       # C++ core + CMakeLists.txt (shared by Android CMake & iOS)
  demux/ decode/ cache/ sched/ convert/ api/ (C ABI for FFI + glue)
third_party/
  versions.lock            # pinned tags (or commit) + sha256 of every shipped upstream; see ADR 0002
  test-deps.lock           # same format, for test-only tools such as GoogleTest; never shipped (ADR 0007)
  build/                   # build-android.sh, build-ios.sh (xcframework), license collector
  prebuilt/                # gitignored; filled by tool/fetch_prebuilts.dart from GitHub Releases
android/                   # Kotlin plugin, JNI bridge, CMake → libwebm_texture.so (core + prebuilt .a; Dart opens it)
ios/webm_texture/          # Package.swift (SwiftPM only); Sources/webm_texture = Swift, Sources/wt_ios = ObjC++ shim, Sources/wt_core = symlinks to src/ (ADR 0009)
example/                   # demo: chat list, picker grid, stress test, integration_test/
test/                      # Dart unit/widget tests
native_test/               # GoogleTest host tests, golden frame checksums
fuzz/                      # libFuzzer targets + seed corpus
tool/                      # fetch_prebuilts, build_host_deps.sh (host tests), gen_corpus.sh (ffmpeg), bench
.github/workflows/         # prebuilt libs, host tests+fuzz, Android/iOS integration, publish dry-run
```

## Phases

### Phase 0: Scaffold + de-risking spikes
1. Run `fvm install 3.47.9 && fvm use 3.47.9`, then `fvm flutter create --template=plugin --platforms=android,ios -a kotlin -i swift --org <org> webm_texture`. The `plugin_ffi` template is not used because Kotlin/Swift texture glue is required.
2. Write `third_party/build` scripts and a CI job that produce static libs for arm64-v8a, armeabi-v7a, x86_64 (and x86, if API 24 emulators still matter) plus an iOS xcframework (device arm64, simulator arm64+x86_64).
   - Android: NDK r30 for the plugin build (ADR 0009), `-Wl,-z,max-page-size=16384` (Play requires 16 KB page alignment).
   - libvpx: `--disable-vp8-encoder --disable-vp9-encoder --enable-vp8-decoder --enable-vp9-decoder --disable-vp9-highbitdepth --size-limit=1024x1024 --enable-pic`, with `--enable-runtime-cpu-detect` on x86. libvpx has no `--disable-encoders` switch ([ADR 0002](docs/decisions/0002-pinning-and-prebuilt-contents.md)).
3. **Spike A — alpha decode.** Generate test WebMs with `ffmpeg -c:v libvpx-vp9 -pix_fmt yuva420p` (and the VP8 equivalent). Confirm that `webm_parser` delivers the `BlockAdditional` payload and that the merged RGBA matches a reference. If `webm_parser` can't do it, write a minimal EBML demuxer instead (Cluster / SimpleBlock / BlockGroup / BlockAdditions only).
   - **Outcome:** both worked, and alpha decoded bit-exact. We chose our own minimal EBML reader for size and for robustness against hostile input ([report](docs/spikes/A-alpha.md), [ADR 0004](docs/decisions/0004-own-webm-demuxer.md)).
4. **Spike B — texture transparency.** On Android, test `SurfaceProducer` under Impeller Vulkan **and** the OpenGLES fallback, on 3+ real devices. On iOS, test `CVPixelBuffer` BGRA. Verify premultiplied alpha composites correctly over non-opaque backgrounds.
   - **Outcome:** every reachable renderer/producer pairing composites premultiplied alpha correctly (±1/255), on emulators and the simulator, which the owner accepted. The `ui.Image` fallback is dropped. Skia + `manual` lifecycle loses the image in the background, so the glue uses `resetInBackground` and redraws in `onSurfaceAvailable` ([report](docs/spikes/B-texture-alpha.md), [ADR 0005](docs/decisions/0005-no-ui-image-fallback.md)).
5. **Spike C — density.** On a low-end Android device and an older iPhone, measure 60 standalone textures against a single atlas texture: raster time, memory, and battery.
   - **Outcome:** the atlas was cheaper on every device and backend (60 stickers: iOS raster p90 2.0 vs 5.9 ms; Android API 37 1.2 vs 18.5 ms, where textures mode couldn't sustain 30 fps updates). Its cost is memory per page, so pages are sized to their content. Grids use atlas mode; standalone stays the default outside a scope. Measured on emulators and the simulator, which the owner accepted ([report](docs/spikes/C-density.md), [ADR 0006](docs/decisions/0006-atlas-for-grids.md)).

**Exit criteria:** alpha is correct on both platforms, and the default grid mode is chosen from the measurements (done: atlas, [ADR 0006](docs/decisions/0006-atlas-for-grids.md)). ~~Fallback if Android textures can't do alpha: a Dart-side `ui.Image` per atlas per tick.~~ Not needed: Spike B showed textures composite alpha correctly ([ADR 0005](docs/decisions/0005-no-ui-image-fallback.md)).

### Phase 1: Native core (host-testable)
- Format sniffing; WebM demux (tracks, `AlphaMode`, `DefaultDuration`/timestamps, per-frame durations); VP8/VP9 color+alpha decode; libyuv scaling to a bucket and YUVA conversion; `probe()`.
- A C ABI in `src/api/wt_api.h` (`wt_source_create_memory/file`, `wt_probe`, `wt_anim_create/destroy`, `wt_anim_seek`, `wt_anim_next_frame(dst, dst_size, stride, format)`, ...), with `ffigen` bindings. Its shape is in [ADR 0008](docs/decisions/0008-c-abi-shape.md).
- GoogleTest on macOS/Linux CI: golden checksums per frame against reference decodes, limit enforcement, and corrupt-file handling. libFuzzer targets for each demuxer and for the full pipeline, run for a short time on every PR and longer nightly.

### Phase 2: Platform textures + standalone widget
- Pigeon API ([ADR 0010](docs/decisions/0010-pigeon-api-shape.md)): `createAnimation(sourceKey, source, bucketW, bucketH) → {animId, textureId}`, `setActive`, `dispose`, `deviceSignals`, plus a `streamErrors` event channel.
- Android: a `SurfaceProducer` per Animation (`resetInBackground`), passed to native code as `ANativeWindow` (`RGBA_8888`). Handle `onSurfaceAvailable`/`onSurfaceCleanup`, because the surface is destroyed in the background and has to be recreated. The JNI glue is compiled into `libwebm_texture.so` with the core ([ADR 0011](docs/decisions/0011-android-texture-glue.md)); its per-animation render threads gave way to the core's scheduler in T3.3 ([ADR 0017](docs/decisions/0017-scheduler.md)).
- iOS: a `FlutterTexture` with a `CVPixelBufferPool` (BGRA, IOSurface, Metal-compatible), where native code writes into the locked buffer. Confirm from which thread `textureFrameAvailable` may be called.
- Dart: `WebmSticker`, `StickerRegistry` (refcounted dedupe by hash+bucket), visibility via a custom render object (viewport intersection + `TickerMode`), app lifecycle handling, and placeholder/error states. Visibility is checked after each frame. Controlled, `autoplay: false` and `loop: false` stickers get a private animation ([ADR 0012](docs/decisions/0012-sticker-widget-visibility-and-playback.md)). A non-looping sticker holds its last frame, and `WebmStickerController.seekToFrame` shows any frame, paused or playing ([ADR 0014](docs/decisions/0014-loop-and-seek.md)).

### Phase 3: Cache, scheduler, tiers
- `.wsc` cache writer/reader, LRU eviction under budget, versioned format, and atomic writes (temp file + rename). The budget is enforced after each write and at startup, LRU on mtime, and the platform glue caches under `<cache dir>/webm_texture` ([ADR 0015](docs/decisions/0015-frame-cache-file-and-abi.md), [ADR 0016](docs/decisions/0016-cache-budget-and-eviction.md)).
- Clock and worker pool, frame dropping, and the global `maxConcurrent` limit, prioritising the most visible stickers. One scheduler per process in `src/sched/`, behind `wt_sched_*`/`wt_player_*` and a `wt_sink` per animation; the visible area reaches it through the Pigeon call `setPriority` ([ADR 0017](docs/decisions/0017-scheduler.md)).
- Tier detection on each platform plus runtime downgrades (thermal state, Low Power Mode): the platforms report `deviceSignals` and push changes on `streamDeviceSignals`, Dart picks the tier and sends it through `setSchedulerOptions` ([ADR 0018](docs/decisions/0018-device-tiers.md)).

### Phase 4: Atlas mode
- `StickerAtlasScope`: slot allocator (shelf packing per bucket), native compositor that writes into one platform buffer, and clipped `Texture` children. This is the documented way to show grids ([ADR 0006](docs/decisions/0006-atlas-for-grids.md)).
- Pages are sized to content (≤ 2048 wide, height rounded to 16 px) and every slot has a ≥ 2-px transparent gutter. The allocator's shelf, reuse and handle rules are in [ADR 0019](docs/decisions/0019-slot-allocator.md): slots never move, and the page shrinks only from the bottom.
- Slots are reclaimed when items scroll out, and the atlas grows (taller page) or splits (new page) when full.
- The compositor is in the core (`wt_atlas_*`, [ADR 0020](docs/decisions/0020-atlas-compositor.md)): workers decode into a RAM frame per slot, and once per scheduler tick each page with new frames locks one buffer, rewrites every live slot and its gutter, and presents. A page that changes size gets a new texture, and the old one is retired until Dart releases it, because a `Texture` stretches its buffer to its box. Pigeon: `createAtlas`, `attachToAtlas`/`detachFromAtlas` (batched), `releaseTextures`, `atlasStats`, `disposeAtlas`.
- `StickerAtlasScope` ([ADR 0021](docs/decisions/0021-atlas-scope-and-content-rate.md)) owns one native atlas. A `WebmSticker` below it holds an animation (and slot) only while some of it is on screen, keyed by scope in the registry, and shows its placeholder until the platform says the slot's first frame reached the page texture (`atlasShownTextures`, `wt_atlas_shown_page`). Attaches are batched once per frame; retired textures are released two frames after nothing draws them.
- Pages present at the content rate: a page's players show their frames on a shared beat (its shortest member frame, at least a clock period), so players started at different times don't make a page present at every tick. Slots draw the page texture frozen, and the scope latches each page's newest image once per frame, because Android's external texture asks for a new image on every un-frozen paint.

### Phase 5: Animated WebP + GIF
- `WebPAnimDecoder` (RGBA canvas), converted to the same YUVA cache path ([ADR 0022](docs/decisions/0022-animated-webp.md)): a `CanvasDecoder` seam that GIF also uses, `RgbaToYuva` in full-range BT.601, frame durations of ≤ 10 ms played as 100 ms, a still WebP as one frame, and a WebP-specific 1:1 colour bar (40 dB, measured 40.83) because the 4:2:0 round trip loses chroma detail WebM sources never had.
- giflib plus our own frame compositing ([ADR 0023](docs/decisions/0023-gif.md)): a sequential low-level reader (never `DGifSlurp`) behind the same `CanvasDecoder` seam, disposal none/background/previous with a transparent canvas, the transparency index and local palettes, de-interlacing, rects clipped to the screen, delays under 20 ms played as 100 ms, and rect areas counted against `max_total_pixels` (each rect at most `max_dimension`², [ADR 0025](docs/decisions/0025-security-review-hardening.md)). Its 1:1 colour meets the shared bar (50.01 dB measured), but at buckets whose chroma can't resolve pixel-aligned saturated edges the colour bar is GIF-specific (14 dB, measured 15.26).

### Phase 6: Hardening + pub.dev release
- Integration tests in `example/`:
  - golden screenshots of transparent stickers over a checkerboard on emulator/simulator and Firebase Test Lab real devices
  - `flutter drive --profile` timeline budgets for scrolling a 100-sticker grid
  - memory ceilings
- Docs, example app, `CHANGELOG`, aggregated `LICENSE`/NOTICE for all bundled libraries, iOS `PrivacyInfo.xcprivacy`, `pana` score, and a `flutter pub publish --dry-run` gate.
- Release process: tag → CI builds prebuilts → `tool/vendor_prebuilts.dart` fetches (sha256) and vendors them into the archive, checking size limits → pana → `flutter pub publish --dry-run` → publish via pub.dev OIDC behind an approval gate, off by default ([ADR 0024](docs/decisions/0024-publish-pipeline.md)). A scheduled job watches upstream libvpx/libwebp CVEs and opens bump PRs.

## Key risks
| Risk | Mitigation |
|---|---|
| Android `Texture` alpha/premultiply differs between backends | Spike B found no difference ([ADR 0005](docs/decisions/0005-no-ui-image-fallback.md)); the remaining risk is surface lifecycle, handled in T2.2 |
| Too many platform textures in grids | Grids use atlas mode (Phase 4, [ADR 0006](docs/decisions/0006-atlas-for-grids.md)) |
| Atlas graphics memory (16 MB per 2048² buffer × queue depth) | Content-sized pages, freed with the scope; memory reported per page |
| Hostile/malformed user uploads (libvpx has had in-the-wild CVEs) | Size limits, our own limit checks, a demuxer that allocates nothing sized by input (ADR 0004), fuzzing, fast upstream bumps |
| Binary size per ABI | Decoders only; no highbitdepth; `-Oz` + LTO for our core; report size per ABI in CI |
| CocoaPods is legacy in Flutter | SwiftPM only; no podspec ([ADR 0009](docs/decisions/0009-ffi-loading-and-native-wiring.md)) |
| Cache disk usage | Size buckets, YUVA+LZ4, LRU budget, `clearCache()` |

## Still open (non-blocking)
- Package name (`webm_texture` is a placeholder; `webm_texture` vs `webm_sticker` vs a brand name) and the org/bundle id.
- Whether to also support Dart build hooks (`code_assets`) as the native-lib delivery path once the Kotlin/Swift glue no longer needs to link the core directly. Revisit after 1.0.
