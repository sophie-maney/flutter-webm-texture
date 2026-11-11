# 0020: Atlas compositor: slot frames in RAM, one present per page per tick, a texture per page size

- **Status:** Accepted (T4.2)
- **Date:** 2026-11-11

## Context

[ADR 0006](0006-atlas-for-grids.md) made atlas mode the way to show grids: one platform texture per page, pages sized to their content, a ≥ 2-px gutter cleared on every write. [ADR 0019](0019-slot-allocator.md) settled the slot layout. T4.2 puts frames into the slots. Building it settled:

1. How scheduler workers, which draw one player's frame at a time ([ADR 0017](0017-scheduler.md)), produce one present per page per tick, when a page's buffers come back from the queue full of stale frames (spike C).
2. How a page grows or shrinks when a `Texture` widget stretches whatever buffer it gets to its box, so Dart's layout and the buffer size must change together.
3. How an animation moves between its own texture and an atlas without losing its place, leaking, or showing a half-written frame.
4. What the Dart side (T4.3's `StickerAtlasScope`) gets to work with.

## Decision

### Core (`src/atlas/compositor.{h,cc}`, `wt_atlas_*`, `WT_API_VERSION` 5)

1. **A CPU frame per slot.** An attached player's sink writes each frame into a spare, slot-sized RAM buffer, swapped in as the slot's latest frame on release. Presents read only those latest frames, so a present running next to a decode never sees half a frame. The alternative, decoding straight into the locked page buffer, would hold the buffer across a whole round and still need a copy of every slot that drew nothing this tick, read back from graphics memory.
2. **One present per page per tick.** The scheduler gains *present groups*: a group is a page; the jobs one round dispatches for a group's players form a batch; when the batch's last job finishes (drawn, failed, or dropped), a worker calls the page's `Present`. Presents run ahead of frame jobs, never twice at once for one page, and at most once per clock period (`max_fps`) per page; batches that finish meanwhile are folded into the next present. A request (the first frame after attach, a seek, a new target) presents too, within the same limit. `Present` answers `WT_BUSY` (retry a period later), `WT_NO_TARGET` (wait for a new target) or an error (retry in 100 ms), as sinks do.
3. **A present rewrites every live slot.** It locks one buffer for the whole page (`ANativeWindow_lock` with a NULL dirty rect, a fresh pool buffer on iOS), writes every live slot's whole outer rect (the frame, or nothing before the first one, and the cleared gutter), and shows it. Slots below the target's height are skipped: the texture that shows them is the next one (5). Space outside live slots is not written; nothing samples it.
4. **Switching targets.** `Scheduler::Retarget` moves a player to another sink and group, waiting for its frame in flight, and redraws without a seek where it can. `wt_atlas_attach` retargets a player to a slot and remembers its own sink; `wt_atlas_detach` and `wt_atlas_destroy` give it back; `wt_player_destroy` frees the slot (`PresentTarget::Leave`). Decode errors still reach the player's own sink's `error`. Making this hold under the stress test fixed a timeline bug that standalone redraws had too: a redraw that made the next frame due, followed by a pause before that frame was drawn, left the new target empty. `Timeline::Stop` now requests the shown frame instead.
5. **Pages want a size; the glue gives them textures.** A page's buffers have a fixed size. `wt_atlas_page_info.wanted_height` is the rows in use, rounded to 16 px by the allocator. A page shrinks only once at least `max(128, height / 4)` rows are free, so scrolling doesn't flip-flop it, and wants nothing when it is empty. When it differs from the current target's height, the glue makes a **new platform texture** of that height and passes it with `wt_atlas_set_target`. The old texture keeps its last frame on screen until Dart releases it. Resizing one texture in place would draw the old slots squashed or stretched for a frame or two, until Dart re-laid out with the new size.
6. **Placement:** a free slot of the same size on any page first (no page grows), then the first page that can reuse or add rows, then a new page, up to `max_pages` (≤ 16). Past that, `WT_ERR_LIMIT`: the caller keeps the animation standalone.
7. **Locks and threads.** The lock order is atlas mutex → page target → page slots → page stats. Only the atlas mutex is held while waiting for a frame in flight (`Retarget`), and no worker path takes it. `wt_atlas_get_page` never waits for a frame or a present. The target callbacks run on workers and must not call back into the core. Every new function's thread affinity is in `wt_api.h`. `wt_atlas_destroy` must not overlap `wt_player_destroy` of a player in that atlas; the glue detaches such a player first.
8. **Memory accounting**, per page in `wt_atlas_page_info`:
   - graphics memory is distinct buffers × stride × height. Buffers are counted by an id that the target reports: the gralloc address on Android, the IOSurface ID on iOS, as spike C did;
   - RAM is one frame per live slot, plus at most 8 spare frames per page for decodes in flight. An empty page keeps none.

### Glue

9. **Android:** each page target is an `AtlasWindow` (`android/src/main/cpp/atlas_window.cc`) over a `SurfaceProducer` (`resetInBackground`, [ADR 0005](0005-no-ui-image-fallback.md)). `onSurfaceCleanup` detaches the window synchronously and `onSurfaceAvailable` hands over the new surface, which the core fills from the slots' frames at once. One `unlockAndPost` per page per tick makes the `ImageReader` producer schedule the engine frame itself (it does so for every image, [ADR 0011](0011-android-texture-glue.md)), so there is one frame request per page per tick and no explicit `scheduleFrame()` hop. This replaces the plan's wording ("one `scheduleFrame()` per page, batched on one main-thread hop"), which predates ADR 0011. **iOS:** a `WTAtlasPage` (BGRA IOSurface pool, at most 4 buffers, the newest kept for `copyPixelBuffer`) under a Swift `AtlasPageTexture`, which calls `textureFrameAvailable` once per page per present from the worker, as standalone textures do.
10. **Pigeon** (`pigeons/messages.dart`):
    - `createAtlas({width, maxHeight, maxPages}) → atlasId`.
    - `attachToAtlas(atlasId, animIds)` and `detachFromAtlas(animIds)`: batched, so one frame's worth of changes costs at most one new texture per page. Each returns an `AtlasLayoutMessage`: the slots, or for a detach each animation's new texture of its own; every page `{atlasId, pageId, textureId, width, height}` after the call; and `retiredTextureIds`.
    - An animation entering an atlas retires its own texture. Leaving gives it a new one, which gets the current frame at once.
    - `releaseTextures(ids)` frees retired textures once Dart no longer draws them. The glue keeps at most 64 retired textures and frees the oldest past that.
    - `atlasStats(atlasId)`: per-page size, slots, buffers, graphics and CPU bytes, presents, plus the page textures and every texture the plugin holds.
    - `disposeAtlas(atlasId)` retires the page textures and leaves the animations in it without a texture until `detachFromAtlas` (or `dispose`). `dispose` of an animation in an atlas frees its slot; the page is resized at the atlas's next attach or detach.

### Verification

11. Host tests (`native_test/atlas_test.cc`) cover the layout, page sizes and hysteresis, placement and `WT_ERR_LIMIT`, presents into stale padded buffers, targets that answer busy, none or an error, switching, destroying attached players, and the C ABI on a real VP9-alpha animation in both pixel formats (bit-exact against `wt_anim_next_frame`). `AtlasStressTest` runs four drivers churning players across two small atlases each. They:
    - create, destroy, attach, detach and move players, and play, pause, seek and redraw them;
    - lose and restore surfaces;
    - recreate atlases, with a tuner changing the worker count and `max_concurrent` throughout;
    - replace each page's target whenever its wanted height changes.

    A checker verifies every present pixel by pixel: every live slot holds its player's latest frame and every gutter is clear, although each buffer comes back with stale pixels. It also checks that presents of one page are a period apart, and that nothing is called after it was replaced or destroyed, nor out of protocol. At the end, every slot's last present shows the last frame its player drew. It runs under ASan/UBSan and TSan. The CI TSan filter now includes `Atlas`.
12. `example/integration_test/atlas_test.dart` and `atlas_demo_test.dart` run on the API 37 emulator and the iOS simulator. They cover 64 animations through one page texture, the page growing to a new texture, switching all 64 to their own textures and back, `WT_ERR_LIMIT`, and a slot matching the decoded frame at 1:1.

## Consequences

- RAM grows by one frame per slot: 64 KB for a 128-px sticker, 4.2 MB for the 64-sticker demo. A present copies every live slot even when only one changed: about 5 MB per tick for 64 slots of 144². Spike C measured copies as negligible next to lock and post. Per-buffer dirty tracking is possible later if a profile says so.
- A page that changes size has two textures for a moment. Dart must release retired textures; otherwise the glue's cap of 64 bounds the leak.
- T4.3 builds on this. Its scope should:
  - batch attaches and detaches per frame;
  - lay slots out from the latest page table and release retired ids after the frame that stopped drawing them;
  - keep a placeholder until the slot's first present;
  - key the registry so that one animation isn't shown standalone and in an atlas at once (a player has one target).
- Slots never move ([ADR 0019](0019-slot-allocator.md)), so a page can hold rows that a repack would save (`compactable_rows`). No repacking yet.
- Measured with 64 128-px stickers on one 2048×720 page:

  | Device | Buffers | Graphics memory | CPU memory | Presents/s |
  |---|---|---|---|---|
  | Android API 37 emulator | 6 | 34–35 MB | 4.1–4.3 MB | ~60 |
  | iOS simulator | 2–3 | 11–17 MB | 4.3–4.5 MB | ~48 |

  Spike C predicted 5–7 and 2 buffers.
