# 0002: Source pinning and prebuilt contents

- **Status:** Proposed (T0.4)
- **Date:** 2026-11-09

## Context

PLAN.md asks for `third_party/versions.lock` to hold "pinned tags + sha256 of every upstream", and lists the libvpx flags, starting with `--disable-encoders`. Building the iOS xcframework (T0.4) hit three gaps:

1. **libyuv has no releases or stable tarballs.** Chromium's googlesource regenerates `+archive/<rev>.tar.gz` on every request, so the tarball sha256 changes from one download to the next.
2. **`--disable-encoders` does not exist** in libvpx 1.17.0's `configure`, which rejects it as an unknown option.
3. **The lock needs a format** that bash (the build scripts) and Dart (`tool/fetch_prebuilts.dart`, T0.6) can both read.

## Decision

1. **`versions.lock` is a whitespace-separated table** with the columns `name version sha256 url`. Lines starting with `#` are comments. Every row must have a sha256, and `third_party/build/common.sh` refuses any source that doesn't match it.
2. **libyuv is pinned to a commit on `main`.** Its `url` is `git+<repo>` and its `version` is the full commit id. The fetcher does a shallow fetch of that commit, checks that `FETCH_HEAD` matches it, and creates a tarball with `git archive --format=tar --prefix=libyuv/ <commit>`. That output is deterministic for a given commit, so its sha256 is checked like any other tarball. libyuv therefore gets the same sha256 guarantee as the tagged libraries.
3. **libvpx encoders are disabled per codec** with `--disable-vp8-encoder --disable-vp9-encoder`, the equivalent of the intended flag. The build also passes `--disable-libyuv --disable-webm-io`, so libvpx doesn't compile its own copies of libyuv and libwebm, which would duplicate our symbols.
4. **Only what the core uses goes into the bundle:**
   - libvpx: VP8 and VP9 decoders.
   - libwebm: `webm_parser`, plus `mkvparser` and `mkvmuxer`, which upstream links into the same library. `mkvparser` remains an option for Spike A.
   - libyuv: built without JPEG support.
   - libwebp: `libwebpdecoder` and `libwebpdemux` only, with no encoder.
   - giflib: only the decoder sources.
   - lz4: `lz4.c` and `lz4hc.c`.
5. **Two include roots.** libwebm's installed headers expect both `Headers/` and `Headers/webm/` on the include path: `webm_parser` includes `webm/…`, while mkvparser includes `mkvparser/…`. Consumers (the iOS podspec/Package.swift, T2.x) must add both.

## Consequences

- Bumping libyuv means picking a new commit and recomputing the `git archive` sha256. The T6.5 upstream watch needs a commit-based check for libyuv rather than a tag-based one.
- The libyuv sha256 depends on `git archive`'s tar output. That output has been stable across git versions; if it ever changes, the fetch fails closed rather than silently accepting different bytes.
- Because the `webm` library contains mkvmuxer, the bundle includes a small amount of unused muxer code. The app linker dead-strips it.
