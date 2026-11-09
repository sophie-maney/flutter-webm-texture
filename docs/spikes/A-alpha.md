# Spike A: VP9/VP8 alpha demux and decode

- **Task:** T0.7 🛑 · **Status:** done. **Decision:** our own minimal EBML reader ([ADR 0004](../decisions/0004-own-webm-demuxer.md))
- **Date:** 2026-11-09
- **Host:** Apple M4 Max, macOS 27.2, Apple clang 21. The libraries are pinned in `third_party/versions.lock`: libvpx 1.17.0, libwebm 1.0.0.32 and libyuv 1980. libvpx is built decoder-only with `--size-limit=1024x1024`, as for mobile.
- **Code:** [`spikes/alpha/`](../../spikes/alpha/) (throwaway). Reproduce with the commands in [Reproducing](#reproducing).

## Summary

1. **Alpha works end to end.** The colour stream and the `BlockAdditional` (BlockAddID 1) alpha stream are decoded by two libvpx contexts and merged with libyuv `I420AlphaToABGR(attenuate=1)` into premultiplied RGBA.
   - **Alpha is bit-exact on every frame of every corpus WebM** (303 frames). It is also exact on three variants the corpus lacks: VP9 alt-ref (hidden frames in both streams), a live-style remux with unknown sizes, and a BT.709-tagged clip.
   - Colour PSNR is at least 48.5 dB against the references, above the 45 dB that T1.6 will require.
2. **`webm_parser` can surface BlockAdditions**, so the EBML fallback wasn't strictly needed. I prototyped the minimal EBML reader anyway, because the decision needs the comparison. Both demuxers produce byte-identical frame indexes for every corpus file.
3. **`webm_parser` needs caller-side hardening against hostile files.** Without it:
   - a 76-byte file aborts the process (E5);
   - 2 MiB of empty BlockGroups costs 365 MB of RAM (E6).

   The callback guards in the spike fix both. Under ASan/UBSan, 250k mutated inputs produced no findings in either demuxer.
4. **Recommendation: the minimal EBML reader.** It adds about 16× less code to the binary (5 KB vs 86 KB), it allocates nothing that input controls, and it indexes more than 10× faster. It also lets libwebm leave the prebuilts. This contradicted `PLAN.md`, which named `webm_parser`, so it went to a human, who chose it (see [Decision](#decision)).

## Acceptance: alpha vs reference

**Sampled references** (`test_assets/ref`, ADR 0001), from `spikes/alpha/run.sh`. Premultiplied RGBA from libyuv is compared with the reference:

| file | frame | alpha max err | alpha PSNR | RGB max err | RGB PSNR | RGB PSNR, exact premultiply | RGB PSNR, bilinear chroma | colour ≤ alpha |
|---|---|---|---|---|---|---|---|---|
| vp9_alpha_512.webm | 0 | 0 | exact | 4 | 56.22 | 58.65 | 56.04 | yes |
| vp9_alpha_512.webm | 44 | 0 | exact | 4 | 55.28 | 57.04 | 55.25 | yes |
| vp9_alpha_512.webm | 45 | 0 | exact | 4 | 55.26 | 56.98 | 55.24 | yes |
| vp9_alpha_512.webm | 89 | 0 | exact | 4 | 56.09 | 58.65 | 55.84 | yes |
| vp9_opaque_256.webm | 0 | 0 | exact | 4 | 49.24 | 49.24 | 49.24 | yes |
| vp9_opaque_256.webm | 30 | 0 | exact | 4 | 48.65 | 48.65 | 48.65 | yes |
| vp9_opaque_256.webm | 59 | 0 | exact | 4 | 48.54 | 48.54 | 48.54 | yes |
| vp8_alpha_256.webm | 0 | 0 | exact | 4 | 54.68 | 56.09 | 55.03 | yes |
| vp8_alpha_256.webm | 29 | 0 | exact | 4 | 54.91 | 56.18 | 55.31 | yes |
| vp8_alpha_256.webm | 30 | 0 | exact | 4 | 54.89 | 56.12 | 55.29 | yes |
| vp8_alpha_256.webm | 59 | 0 | exact | 4 | 53.36 | 54.34 | 55.02 | yes |
| vp9_alpha_vfr.webm | 0 | 0 | exact | 5 | 53.78 | 54.82 | 55.16 | yes |
| vp9_alpha_vfr.webm | 16 | 0 | exact | 5 | 53.68 | 54.58 | 55.27 | yes |
| vp9_alpha_vfr.webm | 31 | 0 | exact | 4 | 53.96 | 54.95 | 55.24 | yes |
| vp9_odd_301x199.webm | 0 | 0 | exact | 7 | 50.12 | 50.93 | 49.92 | yes |
| vp9_odd_301x199.webm | 15 | 0 | exact | 10 | 50.66 | 51.43 | 50.45 | yes |
| vp9_odd_301x199.webm | 29 | 0 | exact | 7 | 51.68 | 52.60 | 51.46 | yes |
| vp9_single_frame.webm | 0 | 0 | exact | 5 | 54.76 | 56.27 | 55.21 | yes |
| vp9_with_audio.webm | 0 | 0 | exact | 6 | 51.67 | 52.26 | 55.09 | yes |
| vp9_with_audio.webm | 15 | 0 | exact | 5 | 51.53 | 52.05 | 55.22 | yes |
| vp9_with_audio.webm | 29 | 0 | exact | 5 | 52.09 | 52.65 | 55.10 | yes |

`vp9_opaque_256` has no alpha track. Its output alpha is 255 everywhere, which matches the reference. `run.sh` also checks every index against `manifest.json`: status, frame count, `pts_ms`, keyframes, AlphaMode, audio, size and codec.
- `truncated.webm` gives 60 complete frames plus a truncated status.
- `vp9_2048.webm` gives a limit error.
- `garbage.bin` gives an invalid error.

Both demuxers agree on all of these.

**Every frame** (`spikes/alpha/allframes.sh`) is compared against ffmpeg's libvpx decode of the whole clip, premultiplied exactly as `gen_corpus.sh` does it. The last three rows are variants generated into `build/` that are not in the corpus:

| file | frames (ours/ffmpeg) | with alpha | alpha exact | min RGB PSNR | demuxers agree |
|---|---|---|---|---|---|
| vp9_alpha_512.webm | 90/90 | 90 | 90/90 | 55.04 | yes |
| vp9_opaque_256.webm | 60/60 | 0 | 60/60 | 48.48 | yes |
| vp8_alpha_256.webm | 60/60 | 60 | 60/60 | 53.16 | yes |
| vp9_alpha_vfr.webm | 32/32 | 32 | 32/32 | 53.18 | yes |
| vp9_odd_301x199.webm | 30/30 | 30 | 30/30 | 49.83 | yes |
| vp9_single_frame.webm | 1/1 | 1 | 1/1 | 54.76 | yes |
| vp9_with_audio.webm | 30/30 | 30 | 30/30 | 51.02 | yes |
| vp9_alpha_altref.webm (2-pass alt-ref; 8/90 superframes in **both** streams) | 90/90 | 90 | 90/90 | 55.03 | yes |
| vp9_alpha_live.webm (`-c copy -f webm -live 1` to a pipe) | 90/90 | 90 | 90/90 | 55.04 | yes |
| vp9_alpha_bt709.webm (BT.709 tagged) | 90/90 | 90 | 90/90 | **32.25** | yes |

The BT.709 row shows pitfall 7: the spike always uses the BT.601 matrix. Its alpha is still exact.

## Demuxer comparison

| | libwebm `webm_parser` + our callback | Minimal EBML reader |
|---|---|---|
| Surfaces BlockAdditional | Yes. `OnElementBegin(BlockAdditional)` gives the span. BlockAddID is a sibling that may come after the data, so IDs are matched by BlockMore order in `OnBlockGroupEnd`. That bookkeeping is the one awkward part. | Yes, directly. |
| Copies payloads | No, but only if the callback skips the BlockAdditional body. Otherwise every alpha payload is copied into the Cluster DOM. | No. Only spans are recorded. |
| Our code | 379 lines | 593 lines (incl. comments) |
| Third-party code | about 10.5k lines (6.3k src + 4.2k headers) | none |
| Binary size (arm64 `__text`, `-Os`, dead-stripped) | **+86 KB** | **+5 KB** |
| Index time, 90-frame 512 px clip | 34.9 µs | 2.6 µs |
| Index time per input, stress run | 16.5 µs | 0.9 µs |
| Allocations controlled by input | Leaf elements are `vector::resize(declared size)` before any read. Every SimpleBlock and BlockGroup in a Cluster is kept in its DOM until the Cluster ends (176 B per BlockGroup, which can be 2 B on disk). Both are bounded only by caps in our callback. | None. The only allocation is the frame vector, capped at `max_frames`. |
| E5: 2^55-byte leaf in an unknown-size Segment | **SIGABRT** without the cap (`std::bad_alloc` escapes into `-fno-exceptions` code and terminates); `limit` with it | `truncated` |
| E6: 2 MiB of empty BlockGroups | **365 MB** peak RSS without the cap; 11 MB with an element cap of 16 × `max_frames` | 3.4 MB (mostly the input itself) |
| Validates the EBML header and DocType | No. Random bytes come out as "truncated". The wrapper adds the check. | Yes |
| Unknown-size Segment/Cluster (E4, live remux) | OK | OK |
| Truncated file | 60-frame partial index | 60-frame partial index |
| Laced video block (E3) | Rejected by the wrapper | Rejected |
| `-fno-exceptions -fno-rtti` | Compiles. Allocation failure aborts the process. | Compiles. Allocates nothing that input controls. |
| ASan/UBSan mutation stress | 0 findings | 0 findings |
| Upkeep | Upstream code we don't control. Off by default in libwebm's CMake (`ENABLE_WEBM_PARSER=OFF`, "new parser API"). Ships its own fuzzer. | Ours. Needs the T1.7 `fuzz_webm` target, which is planned anyway. |

`mkvparser`, libwebm's other parser, has no BlockAdditions support at all, so it was ruled out.

**Mutation stress** (`run.sh --asan`): random bit flips, truncation, 0xFF runs that create unknown sizes and huge IDs, vint-marker bytes, and chunk duplication or deletion, applied to every corpus WebM and to its unknown-size variant. Each input is indexed by both demuxers, then up to 8 frames are decoded and merged from each index.

| run | inputs | sanitizer findings | same frame count | identical index |
|---|---|---|---|---|
| seed 1 | 50,000 | 0 | 49,788 (99.6%) | 49,784 |
| seed 2 | 200,000 | 0 | 199,256 (99.6%) | 199,250 |

Every disagreement is about strictness on malformed input, and none is a correctness bug:
- `webm_parser` rejects some broken elements that the mini reader simply skips because it doesn't need them (status −1025/−1026/−1027/−1030). The mini reader then indexes more frames.
- The mini reader rejects two Blocks in one BlockGroup, an unknown-size BlockMore, and a missing DocType. `webm_parser` accepts these (DocType defaults to `matroska`).
- Twice, the libwebm wrapper's own consistency check fired ("BlockMore bookkeeping mismatch"): its BlockMore count from `OnElementBegin` didn't match the DOM's `block_mores`, for example when a BlockGroup holds two BlockAdditions. The wrapper failed safely, but this is the fragile part of option A.

Only libvpx and libyuv (not built with sanitizers here) run on the mangled payloads. They returned errors on 698k of 2.15M frame decodes and never crashed. T1.7 fuzzes the full pipeline with every component instrumented.

## Host decode timing

Single-threaded libvpx (one thread per context, as the scheduler will use), libyuv merge to RGBA, mean of 10 loops on fresh decoders:

| file | frames | colour ms | alpha ms | merge ms | total ms (mean) | total ms (p95) |
|---|---|---|---|---|---|---|
| vp9_alpha_512.webm | 90 | 0.134 | 0.070 | 0.132 | 0.336 | 0.366 |
| vp9_opaque_256.webm | 60 | 0.033 | — | 0.021 | 0.054 | 0.086 |
| vp8_alpha_256.webm | 60 | 0.086 | 0.036 | 0.046 | 0.169 | 0.239 |
| vp9_alpha_vfr.webm | 32 | 0.023 | 0.016 | 0.031 | 0.071 | 0.080 |
| vp9_odd_301x199.webm | 30 | 0.036 | 0.027 | 0.042 | 0.105 | 0.114 |
| vp9_single_frame.webm | 1 | 0.112 | 0.069 | 0.046 | 0.227 | 0.252 |
| vp9_with_audio.webm | 30 | 0.015 | 0.014 | 0.018 | 0.047 | 0.057 |

Alpha adds 50–100% on top of colour decode. These clips are smooth synthetic gradients, so real stickers will decode more slowly, and phones are several times slower than an M4 Max. The device numbers that matter come from T0.9 and the Phase 3 benchmarks.

## Pitfalls

Each is backed by an experiment in `alpha_spike experiments` (E1–E9) or by the runs above.

1. **Alpha keyframes.** In the corpus, alpha keyframes line up with colour keyframes (E1), and seeking to frame 45 on fresh decoders is exact. The format doesn't guarantee alignment, though: the streams come from two independent encoders.
   - T1.3 should peek both payloads with `vpx_codec_peek_stream_info` and treat a frame as a random-access point only when the colour frame is a keyframe **and** the alpha frame is too (or the frame has no alpha).
   - For BlockGroups, the container's keyframe flag is "has no ReferenceBlock". It matched the bitstream on every corpus frame, but use the bitstream for seeking.
   - Looping back to frame 0 on the same decoder pair is exact (E1c), so a loop needs no decoder reset.
2. **Missing or broken alpha mid-GOP** (E2: alpha dropped on frame 10).
   - VP9: the alpha decoder fails two frames later with `Corrupt frame detected`.
   - VP8: decoding continues **silently** with wrong alpha (43 dB, max error 50).

   So T1.4 must not keep feeding the alpha decoder after a gap. Proposal: if an AlphaMode=1 frame has no alpha payload, or its alpha decode fails, keep the last good alpha plane and skip alpha decoding until the next alpha keyframe. The other option is to put the animation into an error state. T1.4 or a human picks.
3. **The AlphaMode flag.** Decode alpha only when the track has `AlphaMode = 1`, and ignore BlockAdditions otherwise, as the Matroska spec says. `BlockAddID` defaults to 1 when absent, and both demuxers handle that. Create the alpha context lazily on the first alpha payload, so opaque files pay nothing.
4. **Laced blocks.** BlockAdditions apply to a whole Block, so alpha on a laced video block is ambiguous. ffmpeg never laces video. Both demuxers return `unsupported` (E3).
5. **Hidden frames.**
   - VP9 alt-ref packs hidden frames into superframes, and with alpha it does so in **both** streams (8 of 90 packets each in the 2-pass variant). Each packet still yields exactly one shown frame per stream, so drain `vpx_codec_get_frame` and keep the shown image. The output was exact.
   - libvpx-vp9 only uses alt-ref in 2-pass mode.
   - ffmpeg refuses VP8 + alpha + alt-ref ("Transparency encoding with auto_alt_ref does not work"), so VP8 invisible-frame packets with alpha are untested. The decoder should treat a packet that shows no colour frame as "no new output" rather than an error.
6. **Sizes.**
   - Reject an alpha image whose format or size differs from the colour image (E8).
   - Reject a decoded size that differs from the track's PixelWidth/Height. VP9 can change resolution at keyframes, and a hostile stream can lie.
   - Odd sizes need chroma rounded up. libyuv handles it, and `vp9_odd_301x199` has exact alpha.
7. **Colour space.**
   - VP9 signals its matrix and range in the bitstream: `img->cs` is 3 (SMPTE 170 = BT.601) and `range` is studio in the corpus.
   - VP8 always reports `cs = 0` (unknown), which means BT.601.
   - The BT.709-tagged clip drops to 32 dB RGB under a hardcoded BT.601 matrix. T1.4 must choose libyuv's matrix from `img->cs` and `img->range`: `kYvuH709Constants`, the JPEG constants for full range, the 2020 constants, and BT.601 otherwise.
   - The **alpha plane is used raw**. The alpha stream is tagged studio range, but no range expansion applies, and the exact matches confirm that.
8. **Pixel format.** VP9 profile 1 (4:4:4, 4:2:2, 4:4:0) decodes to a non-I420 `img->fmt`. Profiles 2–3 fail because high bit depth is off. The spike rejects anything that isn't I420. Sticker tools emit profile 0.
9. **libvpx `--size-limit` works as a second line of defence.** If `vp9_2048.webm` is fed past the demuxer limit, libvpx fails with `Dimensions of 2048x2048 beyond allowed size of 1024x1024` (E7).
10. **Premultiply rounding.** libyuv attenuates as `(c·a + 255) >> 8`, not `round(c·a/255)`. That is ±1 on colour and costs 1–2.5 dB PSNR against the exact reference, still ≥ 48.5 dB. Keep libyuv's single pass. Output is always validly premultiplied (colour ≤ alpha).
11. **Chroma upsampling.** libyuv point-samples chroma, while the references use ffmpeg's interpolated chroma. `I420AlphaToARGBMatrixFilter(kFilterBilinear)` gains up to 3.5 dB on some clips (`vp9_with_audio`) and loses about 0.2 dB on others. It isn't needed for T1.6's 45 dB. Opaque files go through `I420ToARGBMatrix`, which has no filter variant in this path.
12. **Unknown sizes.** Live muxers (MediaRecorder, ffmpeg to a pipe) write an unknown-size Segment and Clusters. Both demuxers index these exactly like the original (E4, live remux). An unknown-size Cluster ends at the first element that can't be its child.
13. **Truncation.** Only commit a frame when its whole BlockGroup or SimpleBlock is in bounds. `truncated.webm` is cut inside frame 60's group, whose alpha would be missing. Both demuxers drop it and keep frames 0–59.
14. **ContentEncodings.** Header-stripping compression or encryption on the video track would mean the payload isn't a raw VP8/VP9 frame. Both wrappers return `unsupported`. Not in the corpus.
15. **Other tracks.** Audio blocks are skipped by track number before any payload is touched. `has_audio` comes from the track list.
16. **Timestamps.** pts = (Cluster Timecode + the block's signed 16-bit offset) × TimecodeScale. These matched `manifest.json` for every frame. The mini reader rejects values that would overflow `int64` nanoseconds.

## Decision

**Decided 2026-11-09: B, the minimal EBML reader** ([ADR 0004](../decisions/0004-own-webm-demuxer.md)). The options as they were presented:

- **A. `webm_parser` with a hardened callback** (what `PLAN.md` says today).
  - Mature EBML handling we don't have to write.
  - We would still own the hardening layer: leaf size caps, element-count caps, EBML/DocType checks, skipping BlockAdditional bodies, and BlockMore-order bookkeeping.
  - Each of those guards a behaviour inside a library we don't control. Bumping libwebm would mean re-auditing them.
  - +86 KB per ABI.
- **B. Minimal EBML reader (recommended).**
  - About 600 lines covering exactly the elements we play, with no allocations controlled by input and no third-party dependency.
  - +5 KB.
  - Its correctness rests on our own fuzzing (T1.7 `fuzz_webm`) and the corpus. In the stress run it was never less robust than `webm_parser`.

Follow-up:
1. ADR 0004 and the `PLAN.md` update landed with this report, and T0.7 is ticked.
2. A separate PR drops libwebm from `versions.lock`, the build scripts, `smoke.cc` and `LICENSES.md`, and rebuilds the prebuilts (the release tag changes, ADR 0003). No other component uses libwebm.
3. The checks in the pitfalls above belong in T1.3/T1.4. T1.3 also ships a demux fuzz target.

## Reproducing

```sh
spikes/alpha/build-host-deps.sh       # host libvpx/libwebm/libyuv from versions.lock (~15 s)
spikes/alpha/run.sh                   # verify + experiments + timing + code size
spikes/alpha/run.sh --asan 50000      # same under ASan/UBSan + 50k-input stress
build/spike-alpha-asan/alpha_spike stress test_assets 200000 2
spikes/alpha/allframes.sh             # every frame vs ffmpeg, plus the extra variants
```
