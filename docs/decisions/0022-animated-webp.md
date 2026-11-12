# 0022: Animated WebP through a canvas-decoder seam

- **Status:** Accepted (T5.1)
- **Date:** 2026-11-11

## Context

T5.1 plays animated WebP through the same YUVA cache path as WebM ([PLAN.md](../../PLAN.md), Phase 5). GIF (T5.2) will produce the same kind of data: a full RGBA canvas per frame, composited by the decoder. `WebPAnimDecoder` is sequential and yields a straight-alpha RGBA canvas. WebM's libvpx path yields YUVA 4:2:0 with exact alpha. Four things needed deciding.

## Decision

1. **A canvas-decoder seam.** `CanvasDecoder` (`src/decode/canvas_decoder.h`) is `position()`, `Reset()` and `Next(RgbaView*)`. `WebpDecoder` implements it over `WebPAnimDecoder` (`MODE_RGBA`, single-threaded). `Anim` owns either a `VpxDecoder` or a `CanvasDecoder`; GIF plugs into the second. A seek backwards resets the decoder and decodes forward from frame 0, and every canvas counts against the decode budget (`pass_pixels_`), the replay included. A decode error marks the decoder unhealthy, so the next frame resets and replays instead of trusting its compositing state.
2. **A format-neutral index.** `ProbeSource` fills a `SourceIndex`: per-frame `FrameTiming` (duration, pts, random-access flag) for every format, plus the format's own index. `Anim` reads timing only from there. A canvas decoder has one random-access frame, 0.
3. **RGBA canvas → YUVA** is `RgbaToYuva` (`src/convert/yuva.cc`), not WebP-specific: libyuv `ABGRToJ420` plus `ARGBExtractAlpha`, signalled as **full-range BT.601**. At the source size the canvas converts straight into the output frame; at a bucket it converts into a source-size staging image and `ScaleColor`/`ScaleAlpha` shrink it, as for WebM. Scaling still happens on straight alpha.
4. **Demuxing** uses `WebPDemuxPartial` (no pixels decoded): canvas size, frame count, durations and the alpha flag (VP8X flag or any frame with alpha). Limits are enforced as for WebM: `max_dimension` on the canvas, `max_frames` on the count, `max_total_pixels` on frames × canvas, `max_duration_ms` on the sum.
5. **Truncated files.** `WebPAnimDecoder` can't read a partial file. A file cut inside a frame plays its complete frames: the demuxer copies the bytes up to the end of the last complete frame, patches the RIFF size, checks that a full demux of the copy finds exactly those frames, and the decoder reads the copy. `info.truncated` is set. No complete frame is `WT_ERR_TRUNCATED`; this holds for every prefix of `anim.webp` (a test).
6. **Duration policy.** A frame duration of 10 ms or less (including the 0 that stills and sloppy muxers write) plays for 100 ms, as browsers do for WebP. Everything else is the file's value, so the manifest's durations are exact. pts is the running sum. Durations are always ≥ 1.
7. **A still (non-animated) WebP is a one-frame animation** that shows for 100 ms (the policy above, since its duration is 0).
8. The frame cache needed no change: `WscParams` holds only size, alpha, frame count and the source fingerprint, and WebP's timing is validated against the file like WebM's.

## Colour accuracy

A WebM frame arrives as YUV, so the golden bar (45 dB colour, exact alpha, [ADR 0001](0001-sampled-reference-frames.md)) measures only decode error. A WebP canvas is RGB, so converting it to 4:2:0 loses chroma detail, most on sharp coloured edges. Measured against the premultiplied references of `anim.webp` (16 frames, 0–7 lossless, 8–15 lossy with ALPH), through the C ABI:

| Check | Measured minimum | Bar |
|---|---|---|
| 1:1 colour (sequential and after seeks) | 40.83 dB | **40 dB** (WebM: 45) |
| 1:1 alpha | identical | identical |
| Buckets 0.5 / 0.375 / 0.25, colour | 35.06 dB | 34 dB (shared) |
| Buckets, alpha | 33.06 dB | 30 dB (shared) |

Full-range BT.601 is what keeps the 1:1 loss this small. So only the 1:1 colour bar is WebP-specific (`kWebpMinPsnr` in `native_test/golden_test.cc`); the shared constants are unchanged. Frame 0 sets the minimum; frames 1–15 are all at or above 46.7 dB.

## Consequences

- No new dependency; libwebp's `webpdemux` and `webpdecoder` were already in the prebuilts.
- `WT_API_VERSION` is unchanged: `wt_probe` and `wt_anim_create` now succeed for WebP, and GIF stays `WT_ERR_UNSUPPORTED`.
- `fuzz_webp` covers the demuxer and the decode path (small bucket and source size, seeks and skips). `fuzz_sniff` no longer expects WebP to be rejected.
