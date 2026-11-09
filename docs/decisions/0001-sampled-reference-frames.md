# 0001: Sampled, gzipped reference frames

- **Status:** Proposed (T0.3)
- **Date:** 2026-11-09

## Context

T0.3 asked for raw premultiplied RGBA reference frames at `test_assets/ref/<name>/<frame>.rgba` and a committed corpus under 5 MB. Those two requirements conflict. `vp9_alpha_512.webm` alone decodes to 90 × 512 × 512 × 4 = 94 MB of raw RGBA, and even gzipped it is about 16 MB. The other clips add tens of MB more.

The reference frames feed two consumers. Spike A (T0.7) checks that alpha matches, and the golden tests (T1.6, T5.x) compare frames with a PSNR ≥ 45 dB threshold. Both still have to decode every frame, but neither needs a reference for every frame to catch the bugs that matter.

## Decision

1. **Sample WebM frames.** Keep the first, middle and last frame, each keyframe, and the frame before each keyframe, because that frame has the most accumulated inter-frame drift. `manifest.json` lists the indices in `ref_frames`. GIF and WebP canvases are small, so they keep every frame. That makes 43 reference frames in total.
2. **Gzip each frame.** The path is `ref/<name>/<frame:03d>.rgba.gz`, and the payload is unchanged: raw premultiplied RGBA8888 with stride `width × 4`. Fully transparent pixels premultiply to zero, so they compress well. zlib is available on every host and test platform, as well as in Dart (`dart:io`) and Python.
3. **Use our own premultiply and GIF reference.** The task said to decode with ffmpeg. That still holds for VP8/VP9/WebP, with two corrections:
   - ffmpeg's `premultiply` filter rounds 255 × 255 down to 254, so references use an exact `round(c × a / 255)` instead.
   - ffmpeg's GIF decoder fills "restore to background" with the palette background colour. GIF references are therefore composited by `gif2webp` (giflib) with browser semantics, where the rectangle is cleared to transparent.

The whole corpus is about 2.3 MB.

## Consequences

- Golden tests compare only the sampled frames, though they must decode all frames to reach them. A bug that corrupts only some unsampled frame and then recovers by the next keyframe would go unnoticed. That is unlikely with libvpx, and the fuzzers cover crashes.
- Test code needs zlib to read references. That is a host-only test dependency, not a runtime one.
- The generator also needs `gif2webp` and `jq`. AGENTS.md lists them.
- The GIF decoder (T5.x) must clear "restore to background" to transparent, and its canvas must start transparent.
