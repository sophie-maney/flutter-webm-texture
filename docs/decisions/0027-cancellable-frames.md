# 0027: Decode before taking the sink, and cancel frames that are waited for

- **Status:** Accepted (T6.1 follow-up, fixes S-05)
- **Date:** 2026-11-12

## Context

The 1.0 security review ([`docs/security.md`](../security.md), S-05) found
four calls that wait for a player's frame in flight, all made on the
platform main thread by the glue:

- `wt_player_destroy` (Android `nativeDestroy`; iOS already called `close`
  on a background queue);
- `wt_atlas_attach` and `wt_atlas_detach`, through `Scheduler::Retarget`;
- `wt_sched_set_options` with fewer workers, which joins the workers that
  go away;
- Android's `SurfacePlayer::SetWindow`, which takes the window lock that a
  worker holds from `acquire` to `release`.

A scheduler job used to acquire the sink's buffer first and then call
`wt_anim_next_frame` into it, so the "frame in flight" could be a catch-up
decode from the last keyframe (after a seek, a redraw into a new surface, an
atlas detach), plus the cache file's fsync and an eviction scan, all while
the sink's buffer was held (on Android, the `ANativeWindow` lock and
`window_mu_`). A 600-frame 512 × 512 WebM with one keyframe takes 155 ms
(the review) to 254 ms (a camera-like clip, below) to reach its last frame
on an Apple-silicon Mac, and seconds on a low-end phone.

## Decision

### 1. Decode first, hold the sink only for the write

`FrameSource::Draw` is split into **`Prepare`**, which decodes the frame at
the cursor (catch-up included) into the source's own YUVA frame and advances
the cursor, and **`Write`**, which converts that frame into a buffer
(YUV→RGBA, premultiplied). `wt::Anim` gains `PrepareFrame` and `WriteFrame`;
`NextFrame` (and so `wt_anim_next_frame`) is the two in a row, unchanged. A
job is now:

1. position the cursor (skips or a seek), as before;
2. `Prepare`, with no sink buffer and no lock held;
3. check for cancellation (below);
4. `acquire` → `Write` → `release`.

A frame that fails to decode never reaches the sink: it uses up its turn,
the sink's `error` reports it once, as before, and nothing is acquired. On
Android this also ends posting an unwritten buffer for a decode failure
(part of I-6).

**A decoded frame waits for a buffer.** When `acquire` answers `WT_BUSY`,
`WT_NO_TARGET` or an error, or the job is cancelled after `Prepare`, the
scheduler keeps the frame (`Player::prepared_`). The timeline is told the
cursor is on that frame, so the next job for it (the retry, a redraw) writes
it without decoding it again, and the first skip of a later job passes it
for free. A seek drops it.

### 2. Cancel what is waited for

Each player has a `wt::CancelFlag` (`src/api/cancel.h`), reset when a job is
dispatched. It is set, under the scheduler's lock, by:

- `RemovePlayer` (`wt_player_destroy`);
- `Retarget` (`wt_atlas_attach`, `wt_atlas_detach`, `wt_atlas_destroy`),
  which also drops a job that no worker has started, instead of waiting for
  a worker to free up;
- `SetOptions` with fewer workers, for the jobs running on the workers that
  go away (the clock gives those frames to the workers that stay);
- the scheduler's destructor.

**Cancellation points**, where a job stops and returns `kCancelled` (an
internal status that never crosses the C ABI):

- before each skip, and before `Prepare`, in the scheduler;
- inside `Prepare` and `Skip`: `wt::Anim` checks the flag before each frame
  of a VP8/VP9 catch-up from the keyframe and of a WebP/GIF replay from
  frame 0;
- after `Prepare`, before `acquire`: the last one.

Nothing checks it between `acquire` and `release`, so **a buffer, once
taken, always gets a whole frame**: a cancelled frame is never shown half
written, in a standalone sink or an atlas slot.

A cancelled `Anim` call **moves nothing**: the cursor stays on its target,
and the decoder keeps its position (`next_decode_`, the canvas decoder's
frame) and its decode budget, which isn't reset, so cancelling can't buy an
attacker a second budget. The next job for that frame resumes the catch-up
where it stopped. A cache file completed by a cancelled call isn't finished
(written header, fsync, rename, eviction scan) in that call; the next call
that isn't cancelled finishes it first, and destroying the animation
abandons it as any unfinished file.

### 3. Lifetimes stay as they were

Every waiting call still **waits for the job to finish** before it returns
(`in_flight_` cleared under the scheduler's lock); nothing is handed over to
a worker to free later. So a cancelled job never touches a destroyed player,
animation, sink, window or atlas slot, and every promise in `wt_api.h` ("no
sink callback runs once this returns") holds unchanged. The wait is now
bounded by the longest stretch without a cancellation point:

- one frame's decode (a VP8/VP9 frame of at most 1024 × 1024, one WebP or
  GIF frame);
- or, if the buffer was already taken, one YUV→RGBA write and `release`;
- or a cache file already being finished (fsync of at most 50 MiB at the
  default budget, [ADR 0026](0026-cache-file-cap.md), and one directory
  scan).

### 4. Android destroys off the main thread

`StickerTexture.dispose()` now releases the window on the main thread
(`nativeSetSurface(null)`, which waits for at most one frame being
written), releases the `SurfaceProducer`, and runs `nativeDestroy` on a
background thread (`Dispatchers.IO`), as iOS runs `close`. The plugin first
detaches an animation from its atlas on the main thread, as iOS does, so
`wt_player_destroy` can't overlap `wt_atlas_destroy`.

### Threads: who waits for what, and what is held

| Call | Thread | Waits for | Holds while waiting |
|---|---|---|---|
| `wt_player_destroy` | background (Android `nativeDestroy`, iOS `close`) | the cancelled job to stop | nothing |
| `wt_atlas_attach`, `wt_atlas_detach` | main | the cancelled job to stop | the atlas mutex |
| `wt_atlas_destroy` | main | each member's cancelled job; a present in flight | the atlas mutex |
| `wt_sched_set_options` (fewer workers) | main | the cancelled jobs on the retiring workers | the options mutex |
| `SurfacePlayer::SetWindow` (Android) | main | one lock, write and post in flight | `window_mu_` |
| `wt_atlas_set_target` | main | a present in flight (a copy of every slot) | the atlas mutex, the page's target lock |
| A worker's `Prepare` | worker | — | nothing: no sink buffer, no window, no scheduler lock |
| A worker's `acquire` → `Write` → `release` | worker | — | the sink's buffer (Android: the `ANativeWindow` lock and `window_mu_`; iOS: a locked pool `CVPixelBuffer`; atlas: a slot's spare frame) |

`wt_api.h`, `surface_player.h`, `jni_bridge.cc`, `WTPlayer.h`, `WTAtlas.h`
and `StickerTexture.swift` say the same per function.

## Measurements

Each call issued 20 ms into a catch-up to frame 599 of a camera-like
512 × 512 WebM with one keyframe (600 frames, 2 MB; the catch-up takes
254 ms), release build, Apple-silicon Mac:

| Call | Before | After |
|---|---:|---:|
| `wt_player_destroy` | 208–215 ms | 0.3–0.7 ms |
| `wt_atlas_detach` | 209–219 ms | 0.1–0.6 ms |
| `wt_sched_set_options` (2 → 1 workers, both decoding) | 215–218 ms | 0.9–1.1 ms |

## Verification

`native_test/sched_cancel_test.cc`:

- `AnimCancelTest`: a catch-up (VP9, VP8, WebP) cancelled before or during
  its run moves nothing, and the next call draws exactly the frame an
  uninterrupted decode draws; a cache file completed by a cancelled call is
  finished by the next one.
- `SchedCancelTest`: destroy, atlas attach and detach, and set-options
  return in under 20 ms (the fastest of three trials; each under 500 ms)
  while a ten-second catch-up is in flight, and the cancelled frame is drawn
  afterwards by whoever carries on; the sink isn't acquired during a decode;
  a decoded frame that found no buffer is written at the retry without being
  decoded again.
- `SchedCancelStressTest.CancellationsRaceCompletions`: six drivers churn
  players with small cancellable catch-ups, retargeting them to new sinks,
  seeking, redrawing and making sinks answer busy or no target, while a tuner
  resizes the pool. Sinks check that no frame is torn (every pixel holds the
  frame index), nothing is called after its player was destroyed or
  retargeted away, and the protocol; every surviving player then reaches the
  frame it is asked for. It runs under ASan + UBSan and under TSan (its name
  matches CI's TSan filter).

The existing scheduler and atlas stress tests run unchanged; their fake
sources implement `Prepare` and `Write` instead of `Draw`.

## Consequences

- No ABI change (`WT_API_VERSION` stays 6): `wt_anim_next_frame`, the
  `wt_sink` callbacks and the atlas target keep their signatures. Their
  documented waits are shorter, and a frame that fails to decode no longer
  calls `acquire` and `release`.
- A sink that answers `WT_NO_TARGET` now costs one decoded frame first (the
  decode used to come after `acquire`): one frame per player each time a
  surface goes away or a texture is retired. The frame is kept and drawn if
  the next target wants it.
- A cache file whose last frame is decoded by a job that is then cancelled
  for a removal is abandoned, as if the animation had been destroyed one
  frame earlier; the next view writes it again.
- This supersedes parts of [ADR 0017](0017-scheduler.md) (decision 4's job,
  7's "fewer workers wait for their frame in flight", 8's `SetWindow` wait
  and 9's `close` wait), [ADR 0020](0020-atlas-compositor.md) (decisions 4
  and 7: `Retarget` cancels before it waits) and the consequence in
  [ADR 0011](0011-android-texture-glue.md) that `dispose` blocks the main
  thread for a frame write.
