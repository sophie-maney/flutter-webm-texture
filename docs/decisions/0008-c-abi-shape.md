# 0008: Shape of the C ABI (`src/api/wt_api.h`)

- **Status:** Proposed (T1.1)
- **Date:** 2026-11-09

## Context

`PLAN.md` sketches the C ABI as `wt_probe`, `wt_anim_create/destroy` and `wt_anim_render_into(buffer, stride, format)`. T1.4 asks for `seek(0)` and `next_frame(dst, stride, fmt)`, and T1.5 for limits enforced at every stage. T1.1 fixes the actual signatures. Dart reaches them through ffigen, and the Kotlin/Swift glue through JNI and ObjC++.

## Decision

1. **Two handles.**
   - `wt_source` holds the bytes. It is either a copy of a memory buffer or a read-only mmap of a file. It is immutable and reference counted, so many animations and concurrent probes can share it.
   - `wt_anim` is a playback cursor at one output size. It is not thread-safe. The scheduler already gives each animation at most one frame in flight.
2. **Limits travel with the source.** `wt_source_create_*` takes a `wt_limits*` (NULL for defaults) and checks the size before reading. Everything built on that source uses the same limits, so a probe and a decode can't disagree. A zero field is invalid rather than "unlimited", so a zeroed struct can't switch the limits off.
3. **Frame output replaces `render_into`.**
   - `wt_anim_next_frame(anim, dst, dst_size, stride, format, &frame_info)` decodes the frame at the cursor straight into the caller's buffer and advances. `dst_size` lets the core bounds-check every write.
   - After the last frame it returns `WT_END` (positive, not an error). The caller loops with `wt_anim_seek(anim, 0)` or stops, as the widget's `loop` flag says.
4. **The output size is fixed per animation.** `wt_anim_create(source, w, h)` takes the size bucket (0 x 0 means source size). That matches the "Animation = (hash, bucket)" concept in `PLAN.md`.
5. **Fixed-width codes.** `wt_status`, `wt_format`, `wt_codec` and `wt_pixel_format` are `int32_t` typedefs with anonymous enum constants, not C enums. Their size is then part of the ABI, and ffigen generates plain constants. A generated Dart enum would throw on a code the bindings don't know yet.
   - 0 is OK, positive codes are non-error outcomes and negative codes are errors.
6. **Truncation is not always an error.** Input that ends after at least one complete frame probes and plays, with `wt_info.truncated` set. `WT_ERR_TRUNCATED` means nothing playable was found.
7. **Versioning.** `wt_api_version()` returns `WT_API_VERSION`, which the Dart side checks at load. Host tests pin each struct's layout (`native_test/c_abi_check.c`), so a layout change fails the build until the version is bumped.

Later phases (frame cache, scheduler, atlas) add functions. They don't change these.

## Consequences

- `PLAN.md` Phase 1 now names `wt_anim_next_frame`/`wt_anim_seek` instead of `wt_anim_render_into`.
- T1.2 implements the sources, T1.4 the animation functions, and T1.5 `wt_probe` and limit enforcement. Each replaces a `WT_ERR_UNIMPLEMENTED` stub and its test.
