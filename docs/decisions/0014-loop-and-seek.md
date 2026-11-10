# 0014: Loop control and seeking

- **Status:** Accepted (T2.9)
- **Date:** 2026-11-10

## Context

[ADR 0012](0012-sticker-widget-visibility-and-playback.md) deferred `loop` and `seekToFrame` to T2.9. The Pigeon API ([ADR 0010](0010-pigeon-api-shape.md)) had neither, and both platform glues always looped. Several behaviours weren't pinned down: what a non-looping animation does when it ends and is paused or resumed, whether a seek draws while paused, what happens to an out-of-range frame, and how a non-looping sticker shares its animation.

## Decision

- **Pigeon.** `createAnimation` gains a final `bool loop` argument. A new `seekToFrame(animId, frame)` shows `frame` right away, active or not; an active animation carries on from there, restarting its clock on that frame. The platform rejects a frame outside `0..frameCount-1` with `WT_ERR_INVALID_ARGUMENT` and ignores an unknown `animId`, like `setActive`.
- **End of a non-looping animation.** After the last frame's duration the animation stops and holds that frame. Pausing and resuming doesn't restart it; only a seek does. Android's render thread mirrors the core's cursor so it never locks a window buffer it has no frame for (a locked buffer must be posted). iOS treats `WT_END` from the shim as the end and publishes nothing.
- **Seek while paused** decodes and draws the frame at once. On Android a seek without a window is remembered and drawn when a window arrives. On iOS a full buffer pool retries the draw, and a newer seek cancels the retry.
- **Registry.** `loop` is part of an animation's identity (key, bucket, owner, loop), so a looping and a non-looping animation are never shared. `StickerLease.seekToFrame` forwards to the platform and drops errors, like `setActive`.
- **Widget.** `WebmSticker.loop` defaults to true. A non-looping sticker without a controller gets a private animation, like `autoplay: false`, so each copy plays once on its own. `WebmStickerController.seekToFrame(frame)`:
  - seeks each animation behind the controller's stickers once, even when several stickers share it;
  - clamps `frame` to each sticker's last frame (stickers on one controller can have different lengths) and throws `RangeError` for a negative frame;
  - is held by a sticker whose animation is still being created or replaced, and applied when the new animation arrives.
  A sticker that moves to a new decode size gets a new animation, which starts at frame 0.

## Consequences

- Non-looping stickers each cost a decoder, as controlled ones already did.
- Seeking is fire-and-forget from Dart. The controller has no current-frame query; that would need an event from the platform.
- `texture_test.dart` checks seeks and the end frame against frames decoded independently over FFI, so the example app now lists `ffi` (already a dependency of the plugin) as a dev dependency.
