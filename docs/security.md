# Security

How `webm_texture` handles hostile input, what was reviewed for 1.0 (task
T6.1, 2026-11-12) and what is still open. To report a vulnerability, see
[Reporting a vulnerability](#reporting-a-vulnerability).

## Threat model

Stickers come from other users. The plugin is built on the assumption that
**every byte of a sticker is chosen by an attacker** who wants to corrupt
memory, read memory they shouldn't, crash the app, or make it burn CPU,
memory, disk or battery.

| Actor or input | Trust | Reaches |
|---|---|---|
| Sticker bytes (WebM, WebP, GIF), from a file, memory, an asset or a `StickerFetcher` | **Untrusted.** Any content, any size the app lets through | Sniffing, the demuxers, libvpx, libwebp, giflib, libyuv, the frame cache writer |
| The app (Dart API, `StickerLimits`, `StickerConfig`, source keys, buckets, the fetcher, the cache directory) | Trusted to mean well, not to be careful. Out-of-range values must fail cleanly, not widen a limit or corrupt memory | Every API |
| Cache files (`.wsc`) in the app's cache directory | App-private, but **not trusted**: a crash mid-write, a full disk, storage faults, a backup restore or a second process can leave them truncated, stale or altered | The `.wsc` reader |
| The platform (Flutter engine, `ANativeWindow`, `CVPixelBuffer`, the JVM) | Trusted, but its buffers are checked before they're written | The glue and the compositor |
| Third-party libraries (libvpx, libwebp, giflib, libyuv, lz4), pinned in `third_party/versions.lock` | Trusted code with a history of CVEs on exactly this kind of input | Everything above |

Out of scope: an attacker who can already write the app's private files or
run code in its process, side channels, and the app's own networking (the
plugin ships no HTTP client; `StickerFetcher` is the app's).

## Attack surface

| Component | Code | Untrusted input | Main risks |
|---|---|---|---|
| Byte sources | `src/demux/source.*` | File size, offsets and lengths read from files | Out-of-bounds reads, huge mappings |
| Sniffing | `src/demux/sniff.*` | Magic bytes | Misrouting to a demuxer |
| WebM demuxer (our own EBML reader, [ADR 0004](decisions/0004-own-webm-demuxer.md)) | `src/demux/webm.*` | EBML IDs, vint sizes up to 2^56, unknown sizes, block headers, lacing flags, timestamps | Integer overflow, unbounded index, deep nesting, CPU |
| VP8/VP9 decode, alpha merge | `src/decode/vpx_decoder.*`, libvpx | Bitstreams, frame sizes that differ from the track, colour and alpha streams that disagree | libvpx memory bugs, size or format mismatches written as if they matched |
| WebP | `src/demux/webp.*`, `src/decode/webp_decoder.*`, libwebp | RIFF chunks, canvas and frame rects, durations, truncation | Canvas allocation, frame count, CPU |
| GIF | `src/demux/gif.*`, `src/decode/gif_decoder.*`, giflib | Screen and image descriptors (rects up to 65535²), LZW streams, palettes, disposal | Rect and canvas writes, LZW expansion, `DGifSlurp`-style memory |
| Scaling and conversion | `src/convert/yuva.*`, libyuv | Plane sizes and strides from the decoders; the caller's destination buffer | Buffer overruns, stride math |
| Frame cache | `src/cache/wsc.*`, `src/cache/store.*`, lz4 | `.wsc` headers, tables, LZ4 payloads; directory listings | Decompression bombs, offset math, TOCTOU, deleting the wrong files |
| Scheduler | `src/sched/*` | Frame durations; app calls from any thread | Use after free across threads, CPU spin on tiny durations |
| Atlas | `src/atlas/*` | Player sizes; platform buffers | Slot and gutter writes past the page |
| C ABI | `src/api/*` | Every argument | NULLs, zero sizes, widened limits, ownership mistakes |
| Android glue | `android/src/main/cpp`, Kotlin | `ANativeWindow` buffers, JNI arrays, handles as `long` | Lock/post pairing, buffer geometry, use after free |
| iOS glue | `ios/webm_texture/Sources` | `CVPixelBuffer`s | Lock pairing, `bytesPerRow`, dispose racing the raster thread |
| Dart | `lib/src` | Limits, keys, fetched bytes | FFI truncation of out-of-range ints, native memory lifetimes |

## Mitigations

- **Limits on everything an input decides** (`wt_limits`, `StickerLimits`):
  input size (2 MiB), width and height (1024, which matches libvpx's
  `--size-limit=1024x1024`), frame count (600), total duration (10 s) and a
  decode budget of width × height × frames (160 Mi px). Every demuxer checks
  them while it indexes, before any pixel is decoded, so a file that probes
  OK is inside all of them; the decode path checks the budget again per
  pass, charging what the decoder really does. A zero field is invalid,
  never "unlimited". GIF frame rects count against the budget too, each at
  most `max_dimension`². The canvas formats clamp frame times of 10 ms
  (WebP) or under 20 ms (GIF) to 100 ms, and the scheduler shows no frame for
  less than 10 ms.
- **Our own WebM demuxer** ([ADR 0004](decisions/0004-own-webm-demuxer.md)):
  no recursion, every element bounded by its parent with overflow-free
  arithmetic, unknown sizes only on Segment and Cluster, payloads kept as
  spans into the input (the only allocation is the frame vector, bounded by
  `max_frames`), lacing and `ContentEncodings` refused, and every colour
  and alpha keyframe held to the track's size (libvpx's `--size-limit`
  covers VP9 only).
- **Strict decoder checks:** a keyframe of another size than the track
  never reaches libvpx, and libvpx images must be 8-bit I420 at exactly the
  track size, for colour and for alpha; a decoder waits for a
  keyframe after any gap or error; giflib is used through its sequential
  low-level reader (never `DGifSlurp`) with our own bounds-checked
  compositing; WebPAnimDecoder's canvas must match the index.
- **Bounds-checked writes:** every destination buffer (the caller's, a
  window's, a pixel buffer, an atlas page) is checked against
  `stride × (height − 1) + width × 4` before a byte is written.
- **A self-validating cache:** every `.wsc` header, table and frame has a
  CRC-32; the header must match the source's size, CRC-32 and shape, the
  timing must match the source's index, and LZ4 decompresses only with
  `LZ4_decompress_safe` into a buffer of the exact frame size. Files are
  written to a unique temporary name, fsynced and renamed, never rewritten
  in place. Names are SHA-256 hashes of the key, so a key can't name a path.
  Eviction touches only regular `*.wsc` and `*.wsc.*.tmp` files directly in
  the directory.
- **Fuzzing:** six libFuzzer targets with ASan and UBSan (`fuzz/`), 60 s on
  every PR, 30 min nightly, and about an hour each for this review, with
  allocations over 256 MB reported as findings.
- **Sanitizers:** the whole host test suite runs under ASan + UBSan on every
  PR, and under ThreadSanitizer for the scheduler and atlas.
- **Upstream watch:** pinned versions with sha256s; a scheduled job (T6.5,
  in progress) checks upstream releases and OSV.

## Review for 1.0 (T6.1, 2026-11-12)

Scope: every parser and decoder path in `src/`, the cache, atlas and
scheduler, `wt_api.h` ownership and thread affinity, the JNI, ObjC++ and
Dart FFI boundaries, the sources, fetcher and cache naming, and the limits.
The code was read in full (three areas in parallel, every finding then
checked against the code and, where it mattered, reproduced), each fuzz
target ran for about an hour (33 min on the final code), and the host suite
ran under ASan + UBSan and under TSan. Findings were measured on an Apple-silicon Mac; phones are slower.

### Findings

**Critical 0 · High 1 (fixed) · Medium 3 (1 fixed, 2 open) · Low 9 (5 fixed,
1 mitigated, 2 open, 1 accepted) · Info 13.** Fixed findings have a regression test that
fails without the fix.

| ID | Component | Severity | Finding | Status | Covered by |
|---|---|---|---|---|---|
| S-01 | VP8 decode (libvpx) | **High** | libvpx's `--size-limit` is enforced by the VP9 decoder only. VP8 takes its frame size from each keyframe header (14 bits, up to 16383 × 16383) and allocates four frame buffers at that size before decoding; the core compared sizes only after decoding. An 811-byte WebM with a 16 × 16 track took ~465 MB of RSS per animation (+547 MB for one packet under ASan), enough for an OOM kill with a few on screen. | Fixed: every colour and alpha keyframe must have the track's size (the demuxer refuses the file, `VpxDecoder` the packet, before libvpx sees it), and the colour decoder waits for a right-size keyframe after any mismatch ([ADR 0024](decisions/0024-security-review-hardening.md)) | `VpxDecoderTest.OversizedVp8KeyframeIsRefusedBeforeDecoding`, `WebmDemuxTest.KeyframeSizeMustMatchTheTrack`, seed `fuzz/corpus/webm/vp8_oversized_keyframe.webm`; commit "Bound VP8 keyframe sizes and GIF rect work in the core" |
| S-02 | GIF decode | Medium | giflib expands a frame's whole rect wherever the canvas is, but the decode budget charged the canvas area. One rect could be ~160 Mpx, and after an LZW error every later frame replays the frames before it, so a GIF of about 65 KB on a 1 × 1 canvas could keep a worker busy for as long as it is visible (an estimated 168 Mpx of LZW per frame shown). | Fixed: a rect may cover at most `max_dimension`², and a GIF frame is charged at the larger of the canvas and its rect, so replays run out of budget | `GifLimitTest.RectsAreChargedAsDecoded`, seed `fuzz/corpus/gif/big_rect_then_error.gif`; same commit |
| S-03 | WebM demuxer, limits | Low | With `max_dimension` raised by the app, WebM tracks weren't held to the decodable size (16384) and the decode budget product in `Push()` could wrap (5 frames of 2^62 px passed a 3 × 2^62 budget). App-only. | Fixed: tracks are held to `YuvaImage::kMaxDimension` like WebP and GIF canvases | `WebmDemuxTest.RaisedDimensionLimitStopsAtTheDecodableSize`; same commit |
| S-04 | Frame cache | Medium | A cache file has no size cap. Decoded frames are stored per frame with LZ4, so incompressible frames don't shrink: a 200 KB crafted WebM wrote a **119 MB** `.wsc` (up to ~395 MB, the format's worst case at the default limits). Every view of such a sticker writes that much flash, and once the file is written, eviction deletes every other cache file (it's the newest) and then the file itself (larger than the 200 MiB budget). On 32-bit Android the mapping also costs that much address space. | **Open, needs a product decision.** Options: (a) don't cache an animation whose worst-case file (`WscMaxFileSize`) is over a cap such as `min(budget / 4, 64 MiB)`, and abandon a write that passes it; (b) abandon a write once it passes the whole budget, and evict an oversized new file first (no new number, but a 190 MB file still flushes the rest); (c) a minimum LZ4 ratio. Recommended: (a), as an ADR. Related: S-11 | — |
| S-05 | Scheduler, platform glue | Medium | `wt_player_destroy`, `wt_atlas_attach`/`detach`, `wt_sched_set_options` and the Android `SetWindow` wait for a frame in flight, and the glue calls them on the platform main thread. That "frame" can be a catch-up decode from the last keyframe (a request after a seek, a redraw after the surface came back, an atlas detach), the cache file's fsync and an eviction scan, all while the sink's buffer (and on Android `window_mu_`) is held. A 600-frame 512 × 512 WebM with one keyframe takes 155 ms to reach its last frame here, seconds on a low-end phone; ANRs are unlikely (each call waits for one job) but jank is easy. | **Open, needs a design change.** Options: decode before `acquire` and hold the sink only for the YUV→RGBA pass (split `FrameSource::Draw` into prepare and write); a cancel flag checked by the catch-up loop so removal doesn't wait; calling `nativeDestroy` off the main thread on Android, as iOS already does for `close`. Recommended: the first two, as an ADR | — |
| S-06 | Dart resolver, iOS glue | Low | iOS passes the cache key as a C string (`sourceKey.UTF8String`), so `key:a\u0000x` and `key:a` named the same `.wsc` file; with S-08, the precondition for cache poisoning. Needs a key the sender influences. | Fixed: keys and paths containing NUL are refused | `StickerResolver a key or path with a NUL is refused`; commit "Refuse values FFI and the glue would mangle in Dart" |
| S-07 | Dart resolver | Low | Fetched bytes were SHA-1 hashed on the UI isolate (≈10 ms/MB), kept in the 32-entry resolution cache and copied twice more before the core refused anything over 2 MiB. A fetcher without its own cap (`http.get(url).bodyBytes`) let a sender freeze the UI and pin memory. | Fixed: fetched bytes over 2 MiB are refused with `WT_ERR_LIMIT` before hashing or caching | `StickerResolver fetched bytes over the input limit are refused, not cached`; same commit |
| S-08 | Frame cache | Low | The cache fingerprint is the source's size and CRC-32. If two stickers' keys collide (an app that builds `key:` from sender data, a fetcher that reuses file paths, or S-06), a sender can craft content with the same size, CRC-32 (4 free bytes in a Void element or an unknown chunk), shape and timing as a known sticker and have its frames shown for that one until eviction. ADR 0015/0016 claimed "never shows wrong frames". | **Open.** Fix: a v2 header that stores the first 128 bits of the source's SHA-256 (already in tree); docs corrected meanwhile | — |
| S-09 | Dart FFI | Low | `StickerLimits` fields were stored into `uint32_t`/`uint64_t` modulo 2^32/2^64: `maxDimension: -1` meant 4294967295, i.e. no limit, for `WebmStickers.probe`. App-only. | Fixed: out-of-range fields throw `RangeError` | `probe limits that wt_limits can't hold are refused`; same commit |
| S-10 | Dart FFI, cache | Low | `diskBudgetBytes` was only `assert`ed non-negative; in release builds `-1` reached `wt_cache_set_budget` as `UINT64_MAX` and turned eviction off. App-only. | Fixed: a negative budget throws `RangeError` | `a negative budget is refused at the FFI boundary`; same commit |
| S-11 | Frame cache | Low | If the file just written can't be mapped again (out of address space on 32-bit for S-04-sized files, or a concurrent clear), the next loop writes the whole file again, every loop. | **Open**, to fix with S-04: stop caching when the reopen after `Finish` fails | — |
| S-12 | Byte sources | Low | File sources (and cache files) are mapped; a file truncated in place under a mapping raises SIGBUS. Only the app (a fetcher re-downloading over the same path) can do this. | Mitigated: documented on `StickerSource.file`, `StickerFetchResult.file` and in ADR 0013. Option if it recurs: read sources of at most `max_bytes` into memory instead of mapping them | API docs |
| S-13 | Limits | Low | `wt_limits` has no upper bounds: an app that raises `max_dimension` or `max_total_pixels` gives up the memory bounds (WebP and GIF canvases up to 16384², ~1 GiB). Only the app can do this, and only for `WebmStickers.probe` today: playback always uses the defaults. | Accepted, by design (fields may be loosened); this document and the `StickerLimits` docs say so | — |
| I-1 | Upstream | Info | Advisories checked on 2026-11-12: libvpx 1.17.0 was released after the fix for CVE-2026-1861 (a decoder heap overflow, fixed in Chrome 144 in February 2026) landed upstream, and CVE-2026-2447 is on the encoder side, which isn't built; giflib 6.1.3 includes the CVE-2026-23868 fix (6.1.2), and CVE-2026-26740 is in `egif_lib.c`, which the prebuilts don't compile; nothing found for libwebp 1.6.0, lz4 1.11.0 or the pinned libyuv. No bump needed; T6.5's watch keeps checking | — | — |
| I-2 | Fuzzing | Info | The third-party libraries are linked uninstrumented, so libFuzzer has no coverage inside libvpx, libwebp, giflib, libyuv or lz4, and ASan only sees their heap calls. They rely on upstream fuzzing (OSS-Fuzz covers all five). Follow-up: an instrumented host-deps prefix for fuzz builds | Open (follow-up) | — |
| I-3 | Fuzzing | Info | `fuzz_webm` never sought mid-stream, so the catch-up decode from a random-access frame wasn't fuzzed. | Fixed: it now seeks into the middle and to the end; `tool/fuzz.sh` also caps allocations at 256 MB | commit "Seek in fuzz_webm, cap fuzz allocations, add review seeds" |
| I-4 | Fuzzing | Info | Not fuzzed: `Anim::OpenCache` against a source plus a mutated cache file (the reader itself is, by `fuzz_wsc`), `ByteSource::OpenFile`, scheduler timing with hostile durations (host tests only), `SlotAllocator` (inputs are bucket sizes). Follow-up: a `fuzz_anim_cache` target | Open (follow-up) | — |
| I-5 | WebM playback | Info | WebM has no short-frame policy (WebP and GIF play ≤ 10/20 ms frames as 100 ms); the scheduler shows no frame for under 10 ms. A WebM that can't be cached (one broken frame disables caching) decodes up to ~27 Mpx/s for as long as it is visible, about 1.7× the worst legitimate file | Accepted | — |
| I-6 | Android glue | Info | `SurfacePlayer::Release` and `AtlasWindow::Present` post the locked buffer even when the core wrote nothing into it (a buffer can't be cancelled), so a stale earlier frame of the same app can flash. Visual only | Accepted | — |
| I-7 | iOS glue | Info | `StickerTexture.frameReady` reads `closed` under the lock, then calls `textureFrameAvailable` after unlocking, so a call can follow `unregisterTexture`. Relies on the engine ignoring unknown ids (spike B did) | Accepted | — |
| I-8 | Atlas | Info | `Atlas::Page::Present` checked the buffer size in `size_t`, which a bogus stride could wrap on 32-bit; `CheckPixelBuffer` used 64 bits | Fixed: 64 bits | commit "Bound VP8 keyframe sizes and GIF rect work in the core" |
| I-9 | Atlas | Info | `~Atlas` frees a slot even if returning its player fails, which only documented misuse (destroying a player concurrently) can cause | Accepted | — |
| I-10 | Cache directory | Info | `wt_cache_get_stats` deletes files past the 65,536th like every scan; its doc said it only counts | Fixed: documented | `wt_api.h` |
| I-11 | WebP | Info | `has_alpha` comes from the ALPHA flag and per-frame flags only, so an animated WebP whose frames don't cover the canvas shows black instead of transparent there. Correctness, not security | Open (follow-up) | — |
| I-12 | Decoders | Info | VP8's corrupted-frame flag isn't checked, so a truncated partition shows garbage; libyuv's box filter saturates for reductions beyond ~256× (black output). Memory-safe | Accepted | — |
| I-13 | Docs | Info | `StickerLimits` only affect `WebmStickers.probe`; playback always uses the defaults, which the class docs don't say. The `jni_bridge.cc` header says every function runs on the main thread, but `nativeEvictCache` is called from a background thread (correctly) | Open (T6.2 docs) | — |

Checked and found sound, among others: the EBML reader (no recursion, every
element inside its parent with overflow-free arithmetic, unknown sizes only
on Segment and Cluster, timestamps, lacing), byte sources (`Slice`/`Read`,
`fstat` before `mmap`, `O_NONBLOCK`), the WebP demuxer and its truncated
prefix, giflib use (no `DGifSlurp`, `SavedImages` freed, clipping,
interlacing, palettes, disposal 3 buffer), `yuva.cc` size and stride math,
`CheckPixelBuffer`, the `.wsc` reader (CRCs before trust, exact tiling,
`LZ4_decompress_safe` into the exact frame size) and writer (`O_EXCL` 0600
temporaries, fsync, atomic rename), cache naming (SHA-256 of the key, no path
traversal) and eviction (`fstatat(AT_SYMLINK_NOFOLLOW)`, `unlinkat`), slot
and gutter writes, scheduler lifetimes (no callback after destroy, bounded
queues, no spin), JNI references, exceptions and `ANativeWindow`
lock/post pairing, `CVPixelBuffer` locking and retain counts, and Dart FFI
allocations (arenas released in `finally`, no long-lived native handles).

### Fuzzing

Each target ran for 32 min 37 s on the final code of this review
(`tool/fuzz.sh`, one worker per target, all six in parallel on a 16-core
Apple-silicon Mac, `-malloc_limit_mb=256`), after two shorter runs (10 and 17
min) on earlier revisions whose corpora it continued from. The plan asked for
4 hours a target; the owner decided a shorter run was enough. `cov` and `ft`
count edges and features in our code only (I-2).

| Target | Executions | exec/s | cov | ft | Corpus (in run / files) | Peak RSS | Findings |
|---|---:|---:|---:|---:|---|---:|---|
| `fuzz_sniff` | 570,692,197 | 291,764 | 120 | 182 | 48 / 142 | 521 MB | none |
| `fuzz_probe` | 133,583,639 | 68,294 | 670 | 2,364 | 639 / 6,229 | 756 MB | none |
| `fuzz_webm` | 3,600,496 | 1,840 | 649 | 2,391 | 509 / 1,795 | 691 MB | none |
| `fuzz_webp` | 2,067,453 | 1,056 | 396 | 886 | 66 / 161 | 528 MB | none |
| `fuzz_gif` | 15,899,700 | 8,128 | 565 | 1,730 | 320 / 1,844 | 467 MB | none |
| `fuzz_wsc` | 37,553,025 | 19,198 | 172 | 481 | 122 / 444 | 521 MB | none |

No crash, timeout, leak or OOM in any run. `fuzz_sniff` and `fuzz_wsc`
plateau within minutes: their code is small and fully reached. `fuzz_webm`
gained the mid-stream seeks (I-3); its peak RSS was 1.1 GB in the earlier
runs and 691 MB after the S-01 fix, which stops VP8 from allocating at
keyframe sizes. The fuzzers
didn't find S-01 or S-02 themselves (both came from reading the code): the
allocations stayed under libFuzzer's old 2 GB default, and the harnesses'
small budgets kept S-02's replays under the 10 s timeout.

### Sanitizers

Full host suite (`ctest`, every test, not just the scheduler subset CI runs
under TSan), on the final code of this review:

| Build | Tests | Result |
|---|---|---|
| ASan + UBSan (`-DWT_SANITIZE=ON`) | 327 | 327 passed, no sanitizer report |
| ThreadSanitizer (`-DWT_TSAN=ON`) | 326 | 325 passed, 1 disabled by design (`SanitizerTest.NotASanitizerBuild`), no data race |

### Follow-ups

1. S-04 and S-11: cap cache file size (product decision, ADR).
2. S-05: keep the sink (and the platform thread) out of long decodes (ADR).
3. S-08: a cryptographic source fingerprint in a v2 cache header (ADR).
4. I-2, I-4: instrumented third-party libraries for fuzzing, and a
   `fuzz_anim_cache` target.
5. I-11, I-13: WebP alpha detection; `StickerLimits` and thread docs.

## Reporting a vulnerability

Please report security problems privately, not in a public issue: use
GitHub's private vulnerability reporting ("Report a vulnerability" under the
repository's **Security** tab). Include a sample file if you have one, and
the platform, device and plugin version. Expect an acknowledgement within a
week. Fixes ship in a patch release, and the advisory names the reporter
unless they'd rather it didn't.

Problems in libvpx, libwebp, giflib, libyuv or lz4 themselves should also go
to their maintainers; tell us too, so the pinned version can be bumped.
