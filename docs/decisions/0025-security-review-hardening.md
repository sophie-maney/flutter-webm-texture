# 0024: Hardening from the 1.0 security review

- **Status:** Accepted (T6.1)
- **Date:** 2026-11-12

## Context

The T6.1 review ([`docs/security.md`](../security.md)) found inputs that the
limits in `PLAN.md` didn't bound, because a decoder does more work, or
allocates more, than the index the limits are checked against:

- libvpx's `--size-limit=1024x1024` is enforced by the VP9 decoder only.
  VP8 reads its frame size from each keyframe header (14 bits, up to
  16383 x 16383) and allocates four frame buffers at that size before it
  decodes anything. Our demuxer checked `PixelWidth`/`PixelHeight` against
  `max_dimension`, and the decoder compared the decoded image with the track
  only after libvpx had decoded it. An 811-byte WebM (a 16 x 16 track) took
  ~465 MB of RSS per animation (S-01).
- giflib expands a frame's whole rect, wherever the canvas is. ADR 0023
  bounded the sum of the rect areas per pass, but one rect could be ~160 Mpx,
  and the decode budget charged each frame as the canvas area, so the
  replays after a decode error ran without limit (S-02).
- A caller that raised `max_dimension` could index WebM tracks larger than
  any frame the core can decode, which also let the decode budget product
  overflow (S-03).

## Decision

1. **Every VP8/VP9 keyframe must have the track's size.** The demuxer peeks
   each colour and alpha keyframe while indexing, and a size other than
   `PixelWidth` x `PixelHeight` makes the file `WT_ERR_INVALID_DATA` (it
   could never play: the decoder already refused such frames after decoding
   them). `VpxDecoder` checks every packet the same way before
   `vpx_codec_decode` and returns `WT_ERR_INVALID_DATA`, then waits for a
   keyframe of the right size. VP8 inter frames can't change the size, so
   every VP8 allocation is bounded by `max_dimension`; VP9 stays under
   `--size-limit`.
2. **A GIF frame's rect may cover at most `max_dimension`² pixels**, one
   frame's worth as for WebM and WebP (`WT_ERR_LIMIT` otherwise). Long, thin
   rects that ADR 0023 clips to the canvas still play. The decode budget
   charges a GIF frame at the larger of the canvas and its rect.
3. **WebM tracks are bounded by `YuvaImage::kMaxDimension` (16384)** whatever
   the caller's `max_dimension`, as WebP and GIF canvases already were.
4. **Dart refuses values the C types can't hold** instead of letting FFI
   wrap them: a `StickerLimits` field below 1 (or a 32-bit field above
   2^32 - 1) throws a `RangeError`, and so does a negative
   `diskBudgetBytes` (only an `assert` before, so release builds turned
   eviction off).
5. **The resolver refuses fetched bytes over 2 MiB** (the core's default
   `max_bytes`, which playback always uses) with `WT_ERR_LIMIT` before it
   hashes them on the UI isolate or keeps them in its cache, and **keys and
   paths containing NUL** with an `ArgumentError` (iOS passes them as C
   strings, so two keys could share one cache file).
6. **`tool/fuzz.sh` passes `-malloc_limit_mb=256`**: the harnesses decode at
   most 256 x 256, so a larger allocation is a bomb worth reporting.

## Consequences

- WebM files whose keyframes disagree with the track now fail `probe()`
  instead of failing every frame. No corpus file does.
- GIFs with a single rect over 1 Mpx (at the default limits) are refused.
  Browsers would show them; no real sticker has one.
- `WebmStickers.probe(networkSource, limits: StickerLimits(maxBytes: …))`
  can't raise the limit for fetched bytes above 2 MiB. Playback never could.
- Two medium findings need product decisions and stay open
  ([`docs/security.md`](../security.md), S-04 and S-05): a cap on cache file
  size, and keeping the sink's buffer (and the platform thread waiting on
  it) out of long decodes.
