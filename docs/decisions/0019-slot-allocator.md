# 0019: Atlas slot allocator: shelves per bucket, slots never move

- **Status:** Accepted (T4.1)
- **Date:** 2026-11-11

## Context

[ADR 0006](0006-atlas-for-grids.md) made atlas pages content-sized (≤ 2048 wide, height rounded to 16 px) with a ≥ 2-px transparent gutter on every slot. T4.1 turns that into pure C++ bookkeeping that the compositor (T4.2) and `StickerAtlasScope` (T4.3) build on. The open questions were what a slot's rects are, what happens to freed space, and who grows the page.

## Decision

`wt::SlotAllocator` (`src/atlas/slot_allocator.h`) hands out slots on one page. It has no pixels, no threads and no platform code, and does not touch `wt_api.h`; T4.2 adds the ABI.

1. **Shelves, one bucket each.** The page is a stack of shelves from y = 0. A shelf is as tall as its slots' pitch, holds one bucket (width × height) and fits `floor(page_width / pitch_width)` slots. Slot *i* sits at `x = i × pitch_width`.
2. **Pitch.** `pitch = round_up(bucket + 2 × gutter, 16)` per axis, so every slot's outer rect starts on a 16-px boundary in both axes. The gutter is 2 px by default (clamped to 2..64). The content rect (`inner`) is inset by the gutter on the left and top, so at least `gutter` px of transparent border remain on every side; a 128-px bucket gets a 144-px pitch with 2 px left/top and 14 px right/bottom. The compositor clears the whole outer rect on every write.
3. **Page height is the rows in use.** It equals the bottom of the lowest live slot (0 when empty), always a multiple of 16, and is reported by `page_size()`. The width is an option (default 2048). `max_height` (default 2048) caps growth; callers can lower it to cap memory.
4. **Allocation order.**
   1. The lowest free column of the lowest shelf holding the same bucket.
   2. An empty shelf inside the page: the shortest one that is tall enough, lowest on a tie; a taller one is split and the remainder stays empty.
   3. A new shelf below the last one, which grows the page.
   4. Otherwise `WT_ERR_LIMIT`, with the page unchanged. The caller then adds a page or frees slots. A bucket that can never fit (pitch wider than the page or taller than `max_height`) returns `WT_ERR_INVALID_ARGUMENT` instead.
5. **Free and reclamation.** Freeing marks the column free. A shelf with no live slots becomes empty and merges with empty neighbours; an empty shelf at the bottom is dropped, so the page shrinks (and may cascade). Interior empty shelves stay and are reused as in 4.2.
6. **Slots never move.** The allocator does no compaction itself, because moving a slot changes the rect every widget draws. `stats().compactable_rows` says how many rows a full repack would save (live slots re-packed densely per bucket), so the owner can rebuild the page when that is worth it. Other stats: live, free and unused area, free columns, shelf and empty-shelf counts, empty and largest-empty rows.
7. **Handles.** A `SlotId` is `generation << 16 | (index + 1)`, never 0. Freed or stale handles fail with `WT_ERR_INVALID_ARGUMENT`. At most `(width / 16) × (max_height / 16) ≤ 16384` slots exist, so the 16-bit index suffices and memory is bounded.
8. **Threads.** None: one owner, which synchronizes any shared use itself.
9. **Status codes** reuse `wt_status` (`WT_OK`, `WT_ERR_INVALID_ARGUMENT`, `WT_ERR_LIMIT`), so T4.2 can pass them through the C ABI unchanged.

## Consequences

- No deviation from ADR 0006 or `PLAN.md`; this records the policy.
- The 16-px rounding costs space for 128-px stickers (144² per slot, 79% of the pitch is content). Packing the gutter tighter, say by sharing it between neighbours, would break the 16-px alignment of slot origins; revisit only with a measurement.
- Shelf-only packing wastes rows when many buckets each hold few slots. Grids use few buckets (a handful of sizes), so the plan accepts this; `compactable_rows` is there to measure it in T4.2/T4.3.
- The property test (`native_test/slot_allocator_test.cc`) models every live slot independently and checks, after each operation: no overlaps, bounds, alignment, gutters, page height = lowest bottom, stats, and that a failed `Alloc` was genuinely full.
