# 0015: Frame cache file and its C ABI

- **Status:** Proposed (T3.1)
- **Date:** 2026-11-10

## Context

T3.1 adds the `.wsc` frame cache (`PLAN.md`, "Frame cache"). It has to settle five things the plan leaves open:

- how a `wt_anim` learns where its cache file lives;
- how a stale file is told apart from a valid one;
- what the per-frame CRC costs on every loop;
- when playback switches from decoding to the file;
- whether the platform glue starts caching now or when the disk budget lands (T3.2).

## Decision

1. **The caller names the file.** A new `wt_anim_create_cached(source, w, h, cache_path, &anim)` takes the full path, and `wt_anim_create` is that call with `NULL`. The core has no cache directory or naming scheme of its own. The name is `<hash>_<w>x<h>_v1.wsc`, where `PLAN.md` said `<hash>_<bucket>_v1.wsc`; a bucket is a width and a height. Adding the function bumps `WT_API_VERSION` to 2. T3.2's `clearCache()`/`cacheStats()` will take the directory.
2. **The header fingerprints the source** with its size and CRC-32, besides the output size, alpha and frame count. Each frame's timing must also match the source's index. A `key:` reused for new content, or a file of the wrong shape, is deleted rather than shown. Computing the CRC costs about a millisecond per 2 MB source when an animation is created (on a background thread), and nothing per frame.
3. **Each frame's CRC is checked once per animation**, the first time that frame is read. Writers never modify a file in place; they rename a new one over it. So the bytes under a mapping can't change, and later loops pay only the LZ4 decompress. The header and the table are checked in full when the file is opened.
4. **One clean pass writes, and the same animation switches to the file.** The pass must run from frame 0 to the end with no seek and no decode error. As soon as the file is renamed into place, the animation reads its next loops from it and frees its libvpx decoder. A file that turns out damaged, when opened or on any later frame, is deleted, and playback falls back to decoding without failing the frame. The next clean pass writes a new file.
5. **The platform glue doesn't cache yet.** Android and iOS keep calling `wt_anim_create` until T3.2 adds the disk budget and eviction. Caching without a budget would let the cache directory grow without bound. T3.2 now also covers passing a cache path from the glue (done: [ADR 0016](0016-cache-budget-and-eviction.md)).

The byte layout is specified in [`docs/cache-format.md`](../cache-format.md).

## Consequences

- No user-visible change until T3.2 wires the cache in; the core, the FFI bindings and the host tests have it now.
- Several animations, in one process or several, can share a cache path: temporary names are unique, the rename is atomic, and readers keep the file they mapped.
- A file that is damaged after its CRC was checked, while an animation still has it mapped, would go unnoticed for that animation. In app-private storage that is a disk fault, outside the threat model.
- Stray `*.tmp` files from a crash are removed by eviction ([ADR 0016](0016-cache-budget-and-eviction.md)).
