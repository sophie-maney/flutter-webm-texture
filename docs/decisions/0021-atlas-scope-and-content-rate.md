# 0021: `StickerAtlasScope`, page beats and one latch per page per frame

- **Status:** Accepted (T4.3)
- **Date:** 2026-11-11

## Context

T4.3 puts the atlas compositor ([ADR 0020](0020-atlas-compositor.md)) behind a widget: `StickerAtlasScope(child: GridView.builder(...))`, under which `WebmSticker`s draw a clipped slot of a shared page texture. Its acceptance asks for visual parity with standalone stickers, and, with 60 stickers, that Flutter renders at the content rate (no extra frames) with every tick delivered, on the API 37 emulator and the iOS simulator.

Building it settled five things:

1. Which animations a scope holds, and when a slot is given back.
2. How Dart knows that a slot's frame is on its page texture. Until then the slot's area of the texture holds stale pixels. Those can be another sticker's frame from a recycled buffer, or nothing at all on a new texture. T4.2 left no signal for this.
3. **Pages presented at every tick.** T4.2 measured about 60 page presents/s on the API 37 emulator with 30-fps content. Each player's content clock starts when it starts playing, so a page's 60 players have phases spread over the whole 33-ms frame. The scheduler's rounds (≤ 60/s) then find some of them due in every round, and the page presents every round.
4. **Android re-acquires the texture once per slot.** `ImageExternalTexture::Paint` (engine, Android) calls `AcquireLatestImage` over JNI on every un-frozen paint of a texture; `MarkNewFrameAvailable` is a no-op there. Sixty slots of one page therefore cost sixty JNI calls a frame. One frame can also draw different images in different slots, and the images left queued make the producer schedule extra frames (`hasPendingImages` in `FlutterRenderer`).
5. **How to measure the frame rate.** `LiveTestWidgetsFlutterBinding` calls `platformDispatcher.scheduleFrame()` after every frame under its live frame policies, so an integration test sees about 60 Dart frames/s whatever the app does.

## Decision

### Scope and registry (`lib/src/sticker_atlas_scope.dart`)

1. **The scope owns one native atlas** (2048 px wide; `maxPageHeight` ≤ 2048, `maxPages` ≤ 16, default 4). It creates the atlas with its first attach and disposes it with the scope. On dispose it also disposes every animation acquired for it at once, without a grace period.
2. **The registry is keyed by atlas** (`_EntryId.atlas`), so an animation is either standalone or in one atlas, never both: a player has one target. Stickers in one scope with the same content, bucket, owner and loop mode share an animation and a slot.
3. **A scoped sticker holds its lease only while some of it is on screen** (ADR 0012's visibility: the post-frame check, `Offstage`, slivers and kept-alive items). It acquires the animation on the first frame it is visible and releases it when it is not. The registry's grace period (2 s) keeps the animation and its slot for a quick scroll back. After that, `dispose` frees the slot. Because ADR 0020 resizes a page only at an attach or detach, the scope then sends an empty attach to let the page shrink. Standalone stickers keep their lease while mounted, as before.
4. **Attaches are batched**: animations that join during a frame go into one `attachToAtlas` at the end of it (a post-frame callback). Calls to the platform run one at a time, in order. An animation that gets `WT_ERR_LIMIT` (or finds no atlas) keeps its own texture and draws it whole.
5. **The page table is the latest layout.** A page that changed size has a new texture id. Each slot keeps drawing what it drew until its frame is on the new texture: the old, retired texture at the old page size (slots never move, ADR 0019), or the placeholder.
6. **Readiness.** The new `wt_atlas_shown_page(atlas, player)` returns the page index once a present into the page's *current* target has written a frame of the player's slot. The core numbers each page's presents and records, per slot, the first present that wrote it. It counts a slot only after the target's `present` returned, and resets every slot when the target changes. Pigeon `atlasShownTextures(atlasId, animIds)` maps that to the page's texture id, or -1. The scope polls it every 16 ms while any slot waits (every 256 ms after 60 polls), and a slot switches to its page texture when the answer matches the page table. So the placeholder shows until the slot's first frame.
7. **Retired textures** (an animation's own texture on attach, a page's old size, every page on dispose) are released when no member draws them. The release waits for two frames: the first rebuilds the widgets that switched, and the second starts after that frame was submitted, since the engine unregisters a texture after the raster work queued before it.
8. **Slot widget.** `RenderStickerAtlasSlot` fits the slot into the box like `FittedBox` (`fit`, centred) and pushes one `ClipRectLayer` with one `TextureLayer` of the whole page, placed around the slot. It uses no transform layers.

### One latch per page per frame

9. **Slots draw page textures with `freeze: true`.** The scope paints each page texture that a slot draws un-frozen first, so it latches that frame's image once and every slot draws the same image. The latch must be painted but must not show. The engine skips painting under a transparent opacity (`painting_is_nop`), so the latch is drawn opaque, clipped to a 1-px box. The page is scaled so that the box samples page pixels (x − 1.4 … x − 0.6, y − 1.4 … y − 0.6) of a slot shown on that page. Bilinear samples there read only columns x − 2 and x − 1 and rows y − 2 and y − 1, the slot's gutter (≥ 2 px), which every present clears. There are five such boxes (the scope's corners and centre), so one of them is drawn while any part of the scope shows. The first latches the image; the others find nothing new, which is a cheap JNI call. iOS needs none of this (`copyPixelBuffer` runs only on frame-available), but behaves the same with it.

### Page beats (core, `src/sched/`)

10. **A group's players show their playback frames on a shared beat.** The beat is the group's shortest member frame (`Timeline::min_duration`, over members with more than one frame), and at least a clock period. Beat times are multiples of the beat since the steady clock's epoch, so groups with the same beat (pages of one atlas, or of two) land on the same times. A frame due at *t* is drawn in the round at the beat nearest *t*. Requests (the first frame, seeks, redraws) are not quantised. The content clock is unchanged (`due_ = job.due + duration`), so the average speed is exact. Because no frame is shorter than the beat, two frames of one player never fall on one beat, and none is dropped. A 30-fps WebM (33/34/33 ms) gets a 33-ms beat, so about every 100th beat is empty: no present happens then, and no frame is lost. Mixed rates share the fastest beat: the 40-ms-and-up variable-rate clip lands on the 30-fps clips' beats. A page with a member whose shortest frame is under a period falls back to the period (presents at up to `max_fps`). Standalone players are unchanged.
11. `WT_API_VERSION` is 6 (`wt_atlas_shown_page`).

### Verification

12. Host tests (ASan/UBSan and TSan):
    - `AtlasTest.PagePresentsAtTheContentRate`: twelve 30-fps players started 7 ms apart on a 60-fps scheduler. It fails without beats (about 60 presents/s).
    - `AtlasTest.MixedRatesShareTheFastestBeat`: 33-ms and 40-ms players together.
    - `AtlasTest.ShownPageWaitsForAPresentIntoTheCurrentTarget`: no target, a parked new target, a second page, and detach.
    - The existing atlas and scheduler suites, including the stress tests.
13. Dart widget tests with a fake host (`test/sticker_atlas_scope_test.dart`) cover: batching; the placeholder until the slot is shown; relayout on a new page texture, keeping the old one meanwhile; release timing; giving the slot back on unmount and when scrolled off screen; scope dispose; the `WT_ERR_LIMIT` fallback; registry keying; the latch layers; and the slot geometry.
14. Integration tests:
    - `app_test.dart` compares the Stress page in atlas mode (4 columns, every sticker on its first frame) with the same grid standalone (≤ 1 % of pixels off by more than 24/255) and with a per-platform golden, then checks that the slots animate when playing.
    - `atlas_perf_test.dart` (not in `all_test.dart`) plays 60 visible stickers at the high tier. It counts page presents (`atlasStats`) and Flutter frames: Dart frames on Android, `Rasterizer::DrawToSurfaces` on iOS. Both must be within 10 % of 30/s. iOS raster p90 must be ≤ 3 ms. The binding's frame policy is `benchmark` while it measures, because the live policies schedule a frame after every frame (5). `benchmark` still runs every frame the engine asks for.

## Measurements

60 visible stickers (Stress page, 6 × 10, an animation each, 192-px buckets on one 2048 × 1536 page), high tier, 20-s window, debug builds. Rows 1–3 are the API 37 emulator (`emulator-5554`) with parts of this ADR disabled; rows 4–5 are the final code:

| Device | Build | Page presents/s | Flutter fps | Raster p50 / p90 (ms) | App CPU |
|---|---|---|---|---|---|
| API 37 emulator | no beats (10), with latch (9) | 54.4 | 41.2 | 1.06 / 14.7 | 22 % |
| API 37 emulator | beats, no latch: 60 un-frozen slots | 24.1 | 21.4 | 1.48 / 91.0 | 30 % |
| API 37 emulator | **final** | **30.3** | **30.1** | **0.85 / 1.73** | 16 % |
| iOS simulator (iPhone 18 Pro, iOS 27.2) | **final** | **30.3** | **30.1** † | **1.08 / 1.58** † | 30 % |

† From the engine timeline: texture-only frames report no `FrameTiming` on iOS. Android raster comes from `FrameTiming`.

- At the emulator's own tier (detected as low, 15 fps), the page presented 14.4/s for content capped to 15 fps, as it should.
- Page memory on Android was 3–6 buffers × 12 MB. iOS used 3.
- **Measure the emulator with the Mac unlocked.** While the host screen was locked, every Android run fell to 3 Flutter fps. Raster p50 was about 275 ms, nearly all of it in `SurfaceGLES::WrapOnScreenFBO`, waiting for the emulator to hand back Flutter's own window buffer. The page also cycled 7 buffers instead of 3–6. iOS simulator runs were unaffected.
- Attaching blocks the platform thread for up to one frame in flight per player (T4.2). The perf test records each batched attach's round trip (`attachMs` in `ATLAS_PERF`). The PR lists the numbers.

## Consequences

- A scoped sticker that scrolls back after its grace period is created again: decoder, frame 0, attach, and a placeholder for a few frames. Standalone stickers in a list's cache extent keep their animations; scoped ones give them back as soon as they leave the screen, which bounds atlas memory to what is visible plus the grace period.
- Every animation still gets a texture of its own from `createAnimation` that the attach retires at once (ADR 0020's API). Creating straight into an atlas would save a texture create per sticker. That needs a new Pigeon call; it can come later if a profile shows the cost.
- The latch boxes assume the scope's corners or centre are on screen when any of its stickers are. That holds for a scope around a grid or list that fills its area. A scope that is mostly scrolled away, with only an edge strip showing, would still latch through the two corners on that edge.
- Beats can show a frame up to half a beat early or late, which no one notices at 30 fps. Beats apply only to atlas pages; standalone players keep their own phase, as each has its own texture and frame request.
- Real-device numbers are T3.5's job, like every number here.
