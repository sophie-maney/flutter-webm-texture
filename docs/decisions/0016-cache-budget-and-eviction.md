# 0016: Cache budget, eviction and glue wiring

- **Status:** Accepted (T3.2)
- **Date:** 2026-11-11

## Context

T3.2 bounds the `.wsc` cache directory and makes the Android and iOS glue use it ([ADR 0015](0015-frame-cache-file-and-abi.md) left both open). Open questions: what "least recently used" means when access times are unreliable, who triggers eviction after a write (the core writes the files, the glue doesn't see it), how a sticker key becomes a file name, which files eviction may delete, and how Dart learns the directory.

## Decision

1. **LRU runs on mtime.** `atime` is off or coarse on `noatime`/`relatime` mounts, Android included. A cache hit touches the file's mtime (`utimensat`); a new file has its rename time. Eviction deletes the oldest first, name as tie-break.
2. **The core evicts after every finished write**, down to a process-wide budget (`wt_cache_set_budget`, default 200 MiB, `UINT64_MAX` = unlimited), under one mutex so concurrent writers can't overshoot together. A global rather than a `wt_anim_create_cached` argument: the budget is an app setting, not per animation, and the signature stays as T3.1 shipped it. The animation reopens its new file before evicting, so one larger than the whole budget is deleted but still plays from its mapping.
3. **Startup eviction** is `wt_cache_evict(dir)`, called by each glue when the plugin attaches (default directory) and by `WebmStickers.configure` (chosen directory and budget).
4. **Only our files are touched:** regular files directly in the directory named `*.wsc` or `*.wsc.*.tmp`. Never symlinks, subdirectories or other names, so a mis-pointed `cacheDirectory` can't lose user data. At most 65 536 files are listed per scan, the rest deleted.
5. **Stray temporaries** older than one hour (mtime) are deleted by eviction; `wt_cache_clear` deletes all of them. A live writer keeps writing its file, so only a paused-for-an-hour writer or a clear can take its file away, and then it just stops caching.
6. **File names** come from `wt_cache_file_path(dir, key, w, h)`: `<first 128 bits of SHA-256(key), hex>_<w>x<h>_v1.wsc`, so any key (including `key:` with slashes) is a safe name. The header's content fingerprint still guards against stale files, so a collision could only make two stickers share one file, never show wrong frames. (T6.1: unless the content is crafted to match the fingerprint, which is a CRC-32; see [`docs/security.md`](../security.md), S-08.) SHA-256 is a small in-tree implementation (no new dependency). Both glues call this function; the naming exists once.
7. **A 0 x 0 bucket isn't cached**, since the output size isn't known before probing. Dart always sends real buckets.
8. **Directory ownership.** A new Pigeon call `configureCache(directory?) -> directory` sets (or, with null, reads) the glue's directory, default `webm_texture` under `Context.cacheDir` / `Library/Caches`. The glue creates the directory when it creates an animation. Dart runs `wt_cache_*` over FFI on background isolates with that path: `WebmStickers.configure(StickerConfig(cacheDirectory:, diskBudgetBytes:))`, `cacheStats()`, `clearCache()`. `StickerConfig` starts with these two fields; the tier fields join it in T3.4.
9. **ABI** gains `wt_cache_set_budget`, `wt_cache_get_budget`, `wt_cache_file_path`, `wt_cache_get_stats`, `wt_cache_evict`, `wt_cache_clear` and `wt_cache_stats`. `WT_API_VERSION` is 3.

## Consequences

- The glue now caches: a sticker that plays through once is read from disk afterwards, including in later sessions.
- No fuzz target: the directory code parses no file content, only names from `readdir`, which it matches by suffix. `fuzz_wsc` still covers the file reader.
- Several processes sharing a directory are tolerated (ENOENT on delete is success) but their budgets aren't coordinated.
- `clearCache()` doesn't stop playback; animations with an open file keep it until they end.
