# Frame cache format (`.wsc`, version 1)

A `.wsc` file holds every frame of one animation at one output size (one
size bucket), already decoded and scaled. The core writes one the first time
an animation plays all the way through, and later loops and later sessions
play from it instead of decoding. Each frame then costs an LZ4 decompress
and the YUV-to-RGBA pass. The implementation is `src/cache/wsc.{h,cc}`, and
`src/api/anim.cc` decides when to read and write files.

## Naming

The caller picks the path and passes it to `wt_anim_create_cached`. The name
should identify the content and the size:

```
<cache dir>/<hash>_<width>x<height>_v1.wsc
```

`<hash>` is 32 hex digits taken from the SHA-256 of the sticker's cache key
(`sha1:…` or `key:…`, see `StickerSource`), so any key becomes a safe name.
The platform glue builds the path with `wt_cache_file_path`, using the
animation's output size. `v1` is this format version, so a future
version gets new names and never touches old files. The core doesn't trust
the name: the header records the size and a fingerprint of the source, and a
file that doesn't match them is deleted.

## Layout

All integers are little-endian. Offsets are in bytes from the start of the
file.

```
+--------------------+  0
| header (64 bytes)  |
+--------------------+  64
| frame table        |  frame_count entries of 32 bytes
+--------------------+  64 + 32 * frame_count  (data start)
| frame 0 (LZ4)      |
| frame 1 (LZ4)      |  back to back, in frame order, up to the end of file
| ...                |
+--------------------+  file_size
```

### Header

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 8 | magic | `57 54 53 43 0D 0A 1A 0A` (`"WTSC\r\n\x1A\n"`) |
| 8 | 4 | version | `1` |
| 12 | 4 | header_size | `64` |
| 16 | 4 | width | output width in pixels, ≥ 1 |
| 20 | 4 | height | output height in pixels, ≥ 1 |
| 24 | 4 | frame_count | ≥ 1 |
| 28 | 4 | flags | bit 0: the frames have an alpha plane. Other bits are 0 |
| 32 | 1 | matrix | YUV→RGB matrix: 0 BT.601, 1 BT.709, 2 BT.2020 |
| 33 | 1 | full_range | 0 studio range (Y 16–235), 1 full range |
| 34 | 2 | reserved | 0 |
| 36 | 4 | source_crc | CRC-32 of the whole encoded source file |
| 40 | 8 | source_size | size of the encoded source file |
| 48 | 8 | file_size | size of this file |
| 56 | 4 | table_crc | CRC-32 of the frame table |
| 60 | 4 | header_crc | CRC-32 of bytes 0–59 |

As in PNG, the magic catches files mangled by text-mode or 7-bit transfers.
`source_size` and `source_crc` tie the file to the exact bytes it was decoded
from. A cache key reused for different content therefore never shows stale
frames by accident. CRC-32 is not collision resistant, though: someone who
controls a sticker and can make its key collide with another sticker's (an
app that builds `key:` from data the sender chooses) can craft one with the
same size and CRC and have its frames shown for the other
([`docs/security.md`](security.md), S-08).

### Frame table

One 32-byte entry per frame, in frame order:

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 8 | offset | where the frame's compressed bytes start |
| 8 | 8 | pts_ms | presentation time relative to frame 0, signed (`wt_frame_info.pts_ms`) |
| 16 | 4 | size | compressed size, 1 ≤ size ≤ `LZ4_compressBound(frame_bytes)` |
| 20 | 4 | crc | CRC-32 of the compressed bytes |
| 24 | 4 | duration_ms | ≥ 1 (`wt_frame_info.duration_ms`) |
| 28 | 4 | reserved | 0 |

The timing is stored rather than derived, because the core reports what the
container said, and in a variable-frame-rate file the PTS isn't the sum of
the rounded durations.

### Frames

Each frame is one LZ4 block (`LZ4_compress_default`, no frame header) that
decompresses to exactly `frame_bytes` bytes: the planes of a YUVA 4:2:0 image,
packed with no padding:

```
Y  width × height
U  ⌈width/2⌉ × ⌈height/2⌉
V  ⌈width/2⌉ × ⌈height/2⌉
A  width × height            (only when flags bit 0 is set)
```

That makes `frame_bytes = w·h + 2·⌈w/2⌉·⌈h/2⌉ (+ w·h)`, which is also the
layout of `YuvaImage`, so a frame decompresses straight into it. Samples are
8-bit. Alpha is straight, not premultiplied: premultiplication happens in the
final conversion to RGBA/BGRA, as it does when decoding.

CRC-32 throughout is the IEEE polynomial (reflected `0xEDB88320`), the same
as zlib's `crc32()`.

## Validation

The reader treats a cache file as untrusted input (`fuzz_wsc` fuzzes it). A
file is used only if all of the following hold, and otherwise it is deleted:

1. It is a regular file no larger than `64 + 32·n + n·LZ4_compressBound(frame_bytes)`,
   checked before mapping.
2. The magic and `header_crc` match. A different `version` is rejected too.
3. width, height and frame_count are within the source's `wt_limits`, and
   they match the animation: its output size, whether it has alpha, and its
   frame count. `source_size` and `source_crc` match the source.
4. `file_size` equals the real size (this catches truncated or extended
   files), and `table_crc` matches.
5. The frames tile the data area exactly: frame 0 starts at the data start,
   each next one starts where the previous ended, and the last ends at
   `file_size`. Every `duration_ms` is ≥ 1, and every reserved field is 0.
6. Each frame's `duration_ms` and `pts_ms` equal what the source's own index
   gives.

The table is copied out of the mapping at open, so later reads use the
validated entries. A frame's CRC is checked the first time that frame is
read. The file under a mapping never changes, because writers replace it
rather than rewrite it, so one check per frame per animation is enough, and
steady-state loops pay no CRC cost. LZ4 decoding uses `LZ4_decompress_safe`
with the exact output size. A CRC mismatch or a short decompress counts as
damage: the file is deleted, and that frame and the rest of the pass are
decoded from the source.

## Writing

1. The first pass that plays frame 0 creates `<path>.<pid>.<n>.tmp` with
   `O_CREAT | O_EXCL`, mode 0600, next to the final path. The pid and a
   per-process counter keep concurrent writers apart.
2. As each frame is decoded and scaled, it is packed, compressed, and written
   at the next data offset. Its table entry is kept in memory.
3. After the last frame, the table goes in at offset 64, the header at offset
   0, then `fsync`, `close`, and `rename` over the final path. Last, the
   directory is fsynced, best effort. If the frame was decoded by a call
   that is being cancelled (a scheduler job for a player that is being
   removed or moved, [ADR 0027](decisions/0027-cancellable-frames.md)),
   this step waits for the animation's next call.
4. Playback switches to reading the new file, and the decoder is freed. If
   the new file can't be opened and mapped again, the animation stops
   caching instead (T6.1, S-11).

**Size cap** ([ADR 0026](decisions/0026-cache-file-cap.md)). A file may be at
most a quarter of the disk budget, and never more than 64 MiB (50 MiB at the
default budget). Frames are checked as they are compressed: a frame that
would take the file past the cap abandons it. The writer doesn't even start
when the smallest file the animation could make (every frame at LZ4's best
ratio, `frame_bytes / 256` bytes) is over the cap. Either way the animation
stops caching for its life, and the process remembers the file (by path, for
up to 256 paths) so later animations of the same sticker at that size don't
write it again unless the budget has grown.

The writer is abandoned, and its temporary file unlinked, if:

- the pass is broken by a seek to anything other than the next frame;
- a frame fails to decode (the file is then not cached for this animation);
- a write fails, for example because the disk is full;
- the file would pass the size cap (above); or
- the animation is destroyed first.

A seek back to frame 0 restarts writing. A failure to create the temporary
file stops caching for that animation. A reader that has the old file mapped
keeps it after a rename; the kernel frees it with the last mapping.

Because a file only ever appears complete under its final name, a crash at
any point leaves either the old file, the new file, or a stray `*.tmp` that
eviction deletes (below).

## Budget and eviction

The cache directory is capped by a process-wide disk budget (default 200 MiB,
`wt_cache_set_budget`). The policy is in `src/cache/store.{h,cc}`
([ADR 0016](decisions/0016-cache-budget-and-eviction.md)):

- **What is touched.** Only regular files directly inside the directory named
  `*.wsc` (cache files) or `*.wsc.*.tmp` (writer temporaries). Symlinks,
  subdirectories and any other file are left alone, so a shared directory is
  safe.
- **LRU runs on mtime.** Access times aren't reliable (`noatime`, `relatime`),
  so opening a valid cache file touches its mtime, and a new file has the time
  of its rename. Eviction deletes the oldest first until the total is within
  the budget.
- **When.** After every finished write (the core does this itself), and when
  the app starts (`wt_cache_evict`). Evictions are serialised by a mutex, so
  concurrent writers can't overshoot together; across processes, deleting a
  file that is already gone counts as success.
- **Stray temporaries.** Eviction also deletes writer temporaries not modified
  for an hour: a live writer keeps modifying its file, so these are left by a
  crash. A writer that loses its file anyway fails its `Finish`, which stops
  caching for that animation; it keeps decoding.
- **Open files.** Deleting a file doesn't disturb animations that mapped it.
  An animation whose own new file is evicted keeps playing from its mapping.
- **One file can't flush the rest.** No file is larger than a quarter of the
  budget (the size cap above), and an abandoned write evicts nothing, so a
  new file pushes out at most a quarter of the budget's worth of older ones.
- **Scan bound.** At most 65 536 cache files are listed per scan; files beyond
  that are deleted outright.
- **`clearCache`.** `wt_cache_clear` deletes every cache file and temporary.
