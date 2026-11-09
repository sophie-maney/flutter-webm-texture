# 0004: Our own minimal WebM demuxer instead of libwebm

- **Status:** Accepted (T0.7, human decision at the Spike A gate)
- **Date:** 2026-11-09

## Context

`PLAN.md` named libwebm's `webm_parser` as the WebM demuxer, with a minimal EBML reader as the fallback if it couldn't surface the alpha `BlockAdditional`. Spike A ([report](../spikes/A-alpha.md)) prototyped both. They produced byte-identical frame indexes on the corpus, and alpha decoded bit-exact with either.

`webm_parser` can do the job, but only behind guards the caller has to supply:
- It sizes leaf elements with `vector::resize(declared size)` before reading them. A 76-byte file aborts the process unless the callback caps leaf sizes (E5).
- Its Cluster DOM keeps every block until the Cluster ends. 2 MiB of empty BlockGroups costs 365 MB unless the callback caps the element count (E6).
- It doesn't require an EBML header or check DocType.
- BlockAddIDs have to be matched to BlockMore order by hand.

It also adds 86 KB of arm64 code, against 5 KB for the minimal reader. `mkvparser`, libwebm's other parser, has no BlockAdditions support.

## Decision

T1.3 implements the WebM demuxer as our own minimal EBML reader in `src/demux/`, based on `spikes/alpha/demux_mini_ebml.cc`:
- It reads only the elements sticker playback needs and skips everything else by size.
- It records payloads as byte spans and never copies them.
- It allocates nothing whose size comes from the input except the frame index, which is capped by `max_frames`.
- It uses no recursion.
- It accepts unknown sizes only for Segment and Cluster.

The checks listed under "Pitfalls" in the Spike A report move into T1.3/T1.4.

## Consequences

- We own a parser of hostile input. Under AGENTS.md's security rule, T1.3 ships a demux fuzz target in the same PR, and T1.7 extends it to demux plus decode.
- No other component uses libwebm. A separate PR removes it from `third_party/versions.lock`, the build scripts, `smoke.cc` and `LICENSES.md`. That changes the prebuilt release tag (ADR 0003), so it needs one workflow run. Until then the prebuilts still contain libwebm, which is harmless: the linker drops it.
- Matroska features beyond what stickers use are out of scope and are rejected or skipped: lacing, ContentEncodings, multiple Segments, seeking via Cues.
