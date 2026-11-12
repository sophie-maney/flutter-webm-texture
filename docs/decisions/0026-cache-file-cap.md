# 0026: A cache file may take at most a quarter of the budget

- **Status:** Accepted (T6.1 follow-up, fixes S-04 and S-11)
- **Date:** 2026-11-12

## Context

The 1.0 security review ([`docs/security.md`](../security.md)) found that a
`.wsc` file had no size limit (S-04). Frames are stored one by one as LZ4
blocks of YUVA 4:2:0, so frames that don't compress take their full size:
the review's 200 KB WebM wrote 119 MB, and the format allows up to 395 MB at
the default limits (600 frames of 512 × 512 with alpha). Each view of such a
sticker wrote that much flash. Once written, the file was the newest one in
the directory, so eviction ([ADR 0016](0016-cache-budget-and-eviction.md))
deleted every other cache file and then the file itself, because it alone
was larger than the 200 MiB budget. A related finding (S-11): when the file
just written couldn't be mapped again, the next loop wrote the whole file
again, on every loop.

The review recommended refusing to cache an animation whose worst case
(`WscMaxFileSize`) is over `min(budget / 4, 64 MiB)`. A worst-case check
costs nothing and needs no writing, but it would also refuse legitimate
stickers whose worst case is large and whose real file is small. So before
choosing, both sizes were measured.

### Measurements

`WscMaxFileSize` (worst case) and the size actually written, with no cap,
for every playable corpus clip and for clips made to look like real
stickers, all at the largest bucket (512 px on the longer side, the aspect
kept, as `StickerBuckets.forSize` does). Sizes are in MB (10⁶ bytes), and
"α" marks an alpha plane. The cap at the default budget is 50 MiB (52.4 MB).
Measured on an Apple-silicon Mac with the core's own decoders.

| Clip | Input | Frames | Worst case | Written |
|---|---:|---:|---:|---:|
| **Corpus** (`test_assets/`, smooth gradients) | | | | |
| `vp9_alpha_512.webm` α | 35 KB | 90 | 59.2 | 14.9 |
| `truncated.webm` α | 27 KB | 60 | 39.5 | 9.9 |
| `vp8_alpha_256.webm` α | 42 KB | 60 | 39.5 | 8.3 |
| `vp9_opaque_256.webm` | 6.5 KB | 60 | 23.7 | 6.1 |
| `vp9_alpha_vfr.webm` α | 6.6 KB | 32 | 21.1 | 2.8 |
| `vp9_odd_301x199.webm` α (512 × 338) | 7.8 KB | 30 | 13.0 | 2.8 |
| `vp9_with_audio.webm` α | 9.4 KB | 30 | 19.7 | 2.4 |
| `anim.webp` α | 18 KB | 16 | 10.5 | 1.1 |
| `anim_disposal.gif` α | 0.8 KB | 6 | 3.9 | 0.05 |
| `vp9_single_frame.webm` α | 2.2 KB | 1 | 0.66 | 0.14 |
| **Sticker-like** (generated, not committed; commands below) | | | | |
| Cartoon on transparency, 3 s at 30 fps (Telegram's video sticker spec) α | 167 KB | 90 | 59.2 | 1.7 |
| Camera-like (detail and grain), 3 s at 30 fps | 290 KB | 90 | 35.5 | 22.8 |
| Camera-like cut-out, 3 s at 30 fps α | 359 KB | 90 | 59.2 | 24.3 |
| Camera-like, 3 s at 60 fps | 286 KB | 180 | 71.1 | 41.8 |
| Camera-like, 3 s at 60 fps, 1.5 MB | 1.5 MB | 180 | 71.1 | 40.2 |
| Lossy animated WebP, camera-like, 3 s at 30 fps α | 2.0 MB | 90 | 59.2 | 19.1 |
| Dithered GIF, 2.8 s at 10 fps α | 2.1 MB | 28 | 18.4 | 4.7 |
| Camera-like, 10 s at 60 fps (the longest the limits allow) | 2.0 MB | 600 | 236.9 | 146.3 |
| **Crafted** | | | | |
| As in the review: a 512 px noise keyframe, then 599 repeats α | 424 KB | 600 | 394.8 | 95.0 |
| The regression seed, a 64 px noise keyframe, then 599 repeats α | 35 KB | 600 | 394.8 | 368.4 |

Camera-like content compresses only 1.6–1.8× with LZ4, flat cartoons 35×,
the corpus about 4×. Upscaled noise doesn't compress at all, so a 35 KB file
writes nearly the worst case (it is the seed
`fuzz/corpus/webm/noise_keyframe_static.webm`).

A worst-case check at 50 MiB would refuse `vp9_alpha_512.webm` (14.9 MB
written), the cartoon sticker (1.7 MB), the camera-like cut-out (24.3 MB),
the animated WebP and every 3 s clip at 60 fps: everything with alpha and 90
frames or more at 512 px. That rules it out as the main mechanism.

The clips were made with ffmpeg 9.0.2 (libvpx-vp9) and img2webp:

```sh
orbit='hypot(X-256-120*sin(2*PI*T/3),Y-256-120*cos(2*PI*T/3))'
# Cartoon on transparency
ffmpeg -f lavfi -i "color=c=black:s=512x512:r=30:d=3,format=yuva420p,geq=lum='if(lt($orbit,110),200,if(lt(hypot(X-256,Y-256),60),80,16))':cb='if(lt($orbit,110),60,128)':cr='if(lt($orbit,110),200,128)':a='if(lt($orbit,110)+lt(hypot(X-256,Y-256),60),255,0)'" \
  -c:v libvpx-vp9 -pix_fmt yuva420p -auto-alt-ref 0 -b:v 600k -g 90 cartoon.webm
# Camera-like: a detailed zoom with temporal grain (r=60 and -b:v 5200k for the
# 1.5 MB clip, -t 10 -b:v 1300k for the 10 s one)
ffmpeg -f lavfi -i "mandelbrot=s=512x512:r=30:end_scale=0.05,noise=alls=12:allf=t+u" -t 3 \
  -c:v libvpx-vp9 -pix_fmt yuv420p -b:v 680k -g 90 camera.webm
# Crafted: one keyframe of noise, then 599 unchanged frames
ffmpeg -f lavfi -i "nullsrc=s=64x64,format=yuva420p,geq=lum='random(1)*255':cb='random(2)*255':cr='random(3)*255':a='random(4)*255'" -frames:v 1 noise.png
ffmpeg -loop 1 -framerate 60 -i noise.png -frames:v 600 -c:v libvpx-vp9 -pix_fmt yuva420p \
  -auto-alt-ref 0 -lag-in-frames 0 -crf 40 -b:v 0 -g 1000 -metadata:s:v:0 alpha_mode=1 crafted.webm
```

## Decision

1. **A cache file may take at most `CacheFileCap()` = `min(budget / 4,
   64 MiB)` bytes**: 50 MiB at the default 200 MiB budget, 64 MiB with an
   unlimited one, nothing with a budget of 0 (`src/cache/store.h`).
   - A quarter of the budget, so one file can push out at most a quarter
     of the cache, the way LRU eviction treats any new file, and never the
     rest of it.
   - Never more than 64 MiB, which bounds the flash written for one
     sticker and the address space its mapping takes on 32-bit Android,
     whatever the budget.
   - At the default budget it keeps every clip measured that is 3 s or
     shorter, including camera-like content at 60 fps (41.8 MB, 80% of the
     cap), and every corpus clip (at most 14.9 MB). It refuses the 10 s
     camera-like clip at 60 fps (146 MB: it would take three quarters of
     the budget) and the crafted files.
2. **The writer abandons a file once its real size would pass the cap.**
   `WscWriter::Create` takes the cap, and `Append` checks each compressed
   frame before writing it: past the cap it closes and deletes its
   temporary file and returns `WT_ERR_LIMIT`. A file of exactly the cap is
   written. Nothing is evicted for an abandoned file, since eviction runs
   only after a finished write, so the rest of the cache is untouched.
3. **Worst-case admission only where it can't refuse anything that would
   fit:** `Create` refuses up front, writing nothing, when even the
   *smallest* file the parameters allow (`WscMinFileSize`: every frame at
   LZ4's best ratio, `frame_bytes / 256`) is over the cap. At the default
   budget no valid animation is that large (600 frames of 1024 × 1024 with
   alpha need at least 6 MB); with a small budget it saves writing a file
   that could never be kept.
4. **The animation stops caching after a refusal**, for its whole life: its
   later loops decode, as an animation without a cache path does, and never
   write again (that was S-11 for this case). **The process remembers the
   refusal** (`RefuseCacheFile`): later animations of the same file (the
   same sticker key at the same size) don't start writing, unless the cap
   has grown since. The memory holds 256 paths and forgets the oldest first.
   So one oversized sticker costs one capped write per process, not one per
   view.
5. **An animation that can't map the file it just wrote stops caching**
   (S-11): no address space left, too many open files, or a concurrent
   `clearCache()`. It keeps decoding instead of writing the file again on
   every loop. The directory is still evicted down to the budget, since the
   file may well be there.

Rejected:
- A worst-case check as the main mechanism: refuses common stickers (see
  the measurements).
- The review's option (b), abandoning a write past the whole budget and
  evicting an oversized new file first: needs no new number, but a 190 MB
  file still flushes the rest of the cache.
- A minimum LZ4 ratio: camera-like stickers compress only 1.6×, and noise
  1.0×, so any useful minimum refuses real content.
- Abandoning early from a projection of the first frames' sizes: a
  heuristic that refuses stickers whose opening frames are busier than the
  rest, and saves little once the cap bounds the write.
- Remembering refusals on disk (a marker file or a v2 header flag): a
  format change for a saving of one capped write per process start.

## Consequences

- The `.wsc` format is unchanged. The writer's behaviour is described in
  [`docs/cache-format.md`](../cache-format.md).
- Legitimate stickers longer than about 3 s of camera-like content at 512 px
  and 60 fps aren't cached at the default budget. They decode on every loop,
  as they did before T3.1; the 10 s clip above takes about 1.1 ms a frame
  here. A larger budget raises the cap, up to 64 MiB.
- A hostile sticker now costs at most one write of the cap (50 MiB at the
  default budget) per process and per bucket, and evicts nothing. Many
  different hostile stickers each cost that once, which only a fetcher that
  accepts any key can allow.
- Tests: `WscCapTest` (the writer at, under and over its cap, the minimum
  size against LZ4's best case, the cap formula, the refusal memory),
  `AnimCacheCapTest.CraftedFileStopsAtTheCapAndEvictsNothing` (the seed at
  512 px: the temporary never passes the cap, nothing is left, three other
  files keep their size, mtime and inode, the animation plays its next loop
  by decoding and a second animation doesn't write),
  `AnimCacheCapTest.CorpusIsStillCachedAtTheLargestBucket`,
  `AnimCacheCapTest.AFailedReopenStopsCaching` (S-11) and
  `CacheBudgetTest.AFileOverAQuarterOfTheBudgetIsntWritten`. The budget
  tests now use budgets of four files or more.
- This supersedes [ADR 0016](0016-cache-budget-and-eviction.md)'s note that
  a file larger than the whole budget is written, mapped and deleted: such a
  file is no longer written.
