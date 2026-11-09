# 0006: Atlas mode is the default for grids

- **Status:** Accepted (T0.9, human decision at the Spike C gate)
- **Date:** 2026-11-09

## Context

`PLAN.md` offers two render modes:
- **standalone:** one platform texture per Animation;
- **atlas:** `StickerAtlasScope`, one texture per page with clipped `Texture` children.

Spike C ([report](../spikes/C-density.md)) was to decide which one grids use. It animated 60 and 120 synthetic 128-px stickers at 30 fps both ways for 120 s per configuration. The devices were an Android API 37 emulator (Impeller GLES, Impeller Vulkan, Skia), a 2-vCPU API 29 emulator as a low-end proxy, and the iOS Simulator.

**Speed (60 stickers).** The atlas was cheaper on every device and backend:

| Device | Measure | Textures | Atlas |
|---|---|---|---|
| iOS | raster p90 | 5.9 ms | 2.0 ms |
| iOS | app CPU | 30% | 16% |
| Android API 37 | raster p90 | 18.5 ms | 1.2 ms |
| Android 2-vCPU proxy | Flutter fps | 10.9 | 15.7 |

On the API 37 emulator, textures mode also could not deliver 30 fps updates: 60 lock/post pairs took about 157 ms per tick. The gap widens at 120 stickers.

**Memory.** The atlas costs graphics memory per page rather than per sticker:
- A 2048² page is 32 MB on iOS (2 buffers) and 80–112 MB on Android (5–7 buffers per `ImageReader` queue).
- A 2048×512 page performed the same at a quarter of the memory.

**Bleed.** With slots packed edge to edge, bilinear sampling pulls in pixels from neighbouring slots. A 2-px transparent gutter fixed it.

The spike ran on emulators and the simulator only, at the owner's request, so it has no battery or thermal data. The ratios between modes were consistent; the absolute numbers are not phone budgets.

## Decision

- **Grids use atlas mode.** `StickerAtlasScope` is the documented and recommended way to show stickers in a grid or picker, and the example app's grid pages use it.
- **Standalone stays the default outside a scope.** A `WebmSticker` with no `StickerAtlasScope` above it, such as an inline chat sticker, uses one texture per Animation.
- **Atlas pages are sized to their content, not fixed at 2048².**
  - A page is up to 2048 px wide.
  - Its height is the shelf rows it needs, rounded up to 16 px.
  - It grows by adding rows (re-creating the page) or by adding pages.
  - Buffer memory is pages × page size × queue depth, and the compositor reports it.
- **Every slot has a transparent gutter of at least 2 px.** The compositor clears it on every write, because recycled buffers hold stale contents.
- **The owner accepts the emulator/simulator evidence for this decision.** Real-device numbers come in T3.5.

## Consequences

- T4.1–T4.3 go ahead as planned, with the page-sizing and gutter rules added (see `EXECUTION_PLAN.md`).
- Grids hold atlas memory for as long as the scope lives. A full 2048² page costs as much as roughly 250 standalone 128-px buffers, so pages must not be allocated larger than needed and must be freed when the scope goes away.
- Per-texture overhead remains in standalone mode, so screens that show many stickers without a scope get slower. The docs should say so.
- The sticker count at which standalone stops being cheaper was not measured. If a "standalone below N" rule is wanted later, it needs a measurement and a new ADR.
- Still to confirm on a phone (T3.5): Android per-buffer lock/post cost, and a 10–12 ms raster cluster seen in emulator atlas runs.
