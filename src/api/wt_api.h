/*
 * wt_api.h: the only C ABI of the webm_texture core.
 *
 * Dart (dart:ffi via ffigen) and the platform glue (Android JNI, iOS ObjC++)
 * include this header and nothing else from src/. It is plain C11 and must
 * stay that way: no C++ types, no exceptions across it, every fallible call
 * returns a wt_status.
 *
 * ---------------------------------------------------------------------------
 * Ownership
 * ---------------------------------------------------------------------------
 * - Every handle comes from a *_create function and is released by exactly
 *   one call to the matching *_destroy. Passing NULL to *_destroy does
 *   nothing. Using a handle after destroying it is undefined behaviour.
 * - On failure, a *_create function stores NULL in its out parameter.
 * - wt_source is reference counted inside the core. wt_anim_create takes its
 *   own reference, so the caller may destroy its source handle as soon as
 *   wt_anim_create returns; the bytes stay alive until the last animation
 *   using them is destroyed.
 * - wt_source_create_memory copies the input. The caller keeps ownership of
 *   its buffer and may free it as soon as the call returns.
 * - Pixel buffers passed to wt_anim_next_frame belong to the caller. The core
 *   writes at most dst_size bytes and keeps no pointer to them after the call.
 * - Strings returned by the core (wt_status_string) are static; never free
 *   them.
 *
 * ---------------------------------------------------------------------------
 * Threading
 * ---------------------------------------------------------------------------
 * Each function's comment states its thread affinity using these terms:
 * - "Any thread": cheap, never blocks, safe to call concurrently, including
 *   from the platform main thread and the Flutter UI/raster threads.
 * - "Background thread": may read files and decode, so it can take
 *   milliseconds or more. Never call it on the platform main thread or the
 *   Flutter UI/raster threads; in Dart, call it inside Isolate.run.
 * A wt_source is immutable after creation: any number of threads may use the
 * same source concurrently. A wt_anim is not thread-safe: calls on one
 * animation must not overlap (the caller serialises them, e.g. the scheduler
 * gives each animation at most one frame in flight), but successive calls may
 * come from different threads. Destroying a handle must not overlap any other
 * call that uses it.
 *
 * ---------------------------------------------------------------------------
 * Untrusted input
 * ---------------------------------------------------------------------------
 * Sticker bytes come from other users. The core checks every input against a
 * wt_limits (wt_limits_init_default unless the app overrides it) and turns
 * malformed input into a negative wt_status, never a crash.
 *
 * ---------------------------------------------------------------------------
 * Pixels
 * ---------------------------------------------------------------------------
 * Frames are 8-bit, 4 channels, premultiplied alpha, rows top to bottom.
 * Android writes WT_PIXEL_FORMAT_RGBA8888 and iOS WT_PIXEL_FORMAT_BGRA8888.
 *
 * Status of this header (T1.1): the types and signatures are final for Phase 1.
 * As of T1.5 every function is implemented for WebM, as of T5.1 for animated
 * WebP and as of T5.2 for GIF. WT_ERR_UNIMPLEMENTED is reserved for functions
 * a later phase adds before it fills them in.
 * T3.1 added wt_anim_create_cached (WT_API_VERSION 2), T3.2 the cache
 * directory functions (WT_API_VERSION 3), T3.3 the scheduler (wt_sched,
 * wt_player and the WT_BUSY / WT_NO_TARGET codes, WT_API_VERSION 4), T4.2
 * the atlas compositor (wt_atlas, WT_API_VERSION 5), T4.3
 * wt_atlas_shown_page (WT_API_VERSION 6).
 */
#ifndef WT_API_H_
#define WT_API_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WT_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bumped whenever a type or a signature in this header changes, so the Dart
 * bindings can refuse a mismatched native library at load time.
 */
#define WT_API_VERSION 6

/*
 * Result codes. 0 is success, positive values are non-error outcomes and
 * negative values are errors. wt_status is a fixed-width integer rather than a
 * C enum so its size is part of the ABI and new codes don't break the Dart
 * bindings; treat any unknown negative value as an error.
 */
typedef int32_t wt_status;
enum {
  WT_OK = 0,
  /* wt_anim_next_frame: the last frame was already returned; nothing was
     written. wt_anim_seek(anim, 0) starts the next loop. */
  WT_END = 1,
  /* wt_sink.acquire: no buffer is free right now (e.g. every pool buffer is
     still on screen). The scheduler tries again on its next tick. */
  WT_BUSY = 2,
  /* wt_sink.acquire: there is nothing to draw into (e.g. the surface is
     gone). The scheduler stops drawing the player until wt_player_redraw. */
  WT_NO_TARGET = 3,

  /* A NULL pointer, zero size, unknown enum value or out-of-range number. */
  WT_ERR_INVALID_ARGUMENT = -1,
  /* The function exists but its task hasn't landed yet. */
  WT_ERR_UNIMPLEMENTED = -2,
  /* Not a format the core recognises, or malformed beyond recovery. */
  WT_ERR_INVALID_DATA = -3,
  /* Well formed, but outside what the core plays (e.g. laced video blocks,
     VP9 profile 1-3, encrypted tracks). */
  WT_ERR_UNSUPPORTED = -4,
  /* The input ends before the first playable frame. Input that ends later
     still probes and plays; see wt_info.truncated. */
  WT_ERR_TRUNCATED = -5,
  /* The input exceeds a wt_limits bound (size, dimensions, frames, duration
     or decode budget). */
  WT_ERR_LIMIT = -6,
  /* An allocation failed. */
  WT_ERR_NO_MEMORY = -7,
  /* A file could not be opened, mapped or read. */
  WT_ERR_IO = -8,
  /* The codec rejected a frame. */
  WT_ERR_DECODE = -9,
  /* A bug in the core: an invariant didn't hold. */
  WT_ERR_INTERNAL = -10,
};

/* Container formats, from the input's magic bytes. */
typedef int32_t wt_format;
enum {
  WT_FORMAT_UNKNOWN = 0,
  WT_FORMAT_WEBM = 1,
  WT_FORMAT_WEBP = 2,
  WT_FORMAT_GIF = 3,
};

/* Codec of the frames inside the container. */
typedef int32_t wt_codec;
enum {
  WT_CODEC_UNKNOWN = 0,
  WT_CODEC_VP8 = 1,
  WT_CODEC_VP9 = 2,
  WT_CODEC_WEBP = 3, /* animated WebP (VP8/VP8L frames, decoded by libwebp) */
  WT_CODEC_GIF = 4,
};

/*
 * Output pixel layouts, named by byte order in memory. Both are premultiplied.
 */
typedef int32_t wt_pixel_format;
enum {
  /* R, G, B, A. Android RGBA_8888. libyuv calls this "ABGR". */
  WT_PIXEL_FORMAT_RGBA8888 = 1,
  /* B, G, R, A. iOS kCVPixelFormatType_32BGRA. libyuv calls this "ARGB". */
  WT_PIXEL_FORMAT_BGRA8888 = 2,
};

/*
 * Safety limits for untrusted input. Start from wt_limits_init_default and
 * tighten or loosen single fields. A field set to 0 is invalid, not
 * "unlimited": functions that take a wt_limits return WT_ERR_INVALID_ARGUMENT.
 */
typedef struct wt_limits {
  /* Largest accepted input, in bytes. Default 2 MiB. */
  uint64_t max_bytes;
  /* Decode budget: width x height summed over every frame of one pass, at
     source size. Default 160 Mi px (167,772,160): 600 frames at 512 x 512,
     plus margin. */
  uint64_t max_total_pixels;
  /* Largest accepted width or height, in pixels, of the source and of the
     output. Default 1024, which matches libvpx's --size-limit. */
  uint32_t max_dimension;
  /* Most frames in one animation. Default 600 (10 s at 60 fps). */
  uint32_t max_frames;
  /* Longest total duration, in milliseconds. Default 10000. */
  uint32_t max_duration_ms;
} wt_limits;

/*
 * What wt_probe learns about a source without decoding pixels.
 */
typedef struct wt_info {
  wt_format format;
  wt_codec codec;
  /* Source size, in pixels. */
  uint32_t width;
  uint32_t height;
  uint32_t frame_count;
  /* Sum of every frame's duration, in milliseconds. */
  uint32_t duration_ms;
  /* frame_count * 1000 / duration_ms, or 0 when duration_ms is 0. Average
     only: frames may have different durations. */
  double fps;
  /* Some pixel can be transparent (WebM AlphaMode = 1, a WebP alpha flag, or
     for GIF a transparent index, a first frame that doesn't cover the canvas
     or a disposal that exposes it). False means every pixel is opaque. */
  bool has_alpha;
  /* The container also holds audio. It is ignored, never played. */
  bool has_audio;
  /* The input ended inside a frame. frame_count and duration_ms cover only
     the complete frames before it, which play normally. */
  bool truncated;
} wt_info;

/* Per-frame metadata returned with each decoded frame. */
typedef struct wt_frame_info {
  uint32_t index;       /* 0-based frame number. */
  uint32_t duration_ms; /* How long to show this frame. */
  int64_t pts_ms;       /* Presentation time relative to frame 0. */
} wt_frame_info;

/* What wt_cache_get_stats, wt_cache_evict and wt_cache_clear report. */
typedef struct wt_cache_stats {
  /* Cache (.wsc) files. */
  uint64_t file_count;
  /* Their combined size, in bytes. */
  uint64_t total_bytes;
} wt_cache_stats;

/* Encoded sticker bytes, from memory or a file. Opaque. */
typedef struct wt_source wt_source;

/* A playback cursor that decodes one source at one output size. Opaque. */
typedef struct wt_anim wt_anim;

/* ------------------------------------------------------------------------- */
/* Library                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Returns WT_API_VERSION as the library was built.
 * Thread: any thread.
 */
WT_API int32_t wt_api_version(void);

/*
 * Returns a static, NUL-terminated name for a status code, such as
 * "WT_ERR_LIMIT", or "WT_ERR_?" for a value this build doesn't know.
 * Thread: any thread.
 */
WT_API const char* wt_status_string(wt_status status);

/*
 * Fills *out with the default limits listed in wt_limits. Does nothing when
 * out is NULL.
 * Thread: any thread.
 */
WT_API void wt_limits_init_default(wt_limits* out);

/* ------------------------------------------------------------------------- */
/* Sources                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Creates a source from a copy of size bytes at data.
 *
 * limits: NULL for wt_limits_init_default. The source keeps a copy, and every
 *         later probe or animation on it uses these limits.
 * Returns WT_ERR_LIMIT when size > limits->max_bytes, before copying.
 * Thread: background thread (copies up to max_bytes).
 */
WT_API wt_status wt_source_create_memory(const uint8_t* data, size_t size,
                                         const wt_limits* limits,
                                         wt_source** out_source);

/*
 * Creates a source that maps the file at path (UTF-8) read-only.
 *
 * limits: as for wt_source_create_memory. The file size is checked against
 *         limits->max_bytes before it is mapped.
 * The file must not be truncated or rewritten while the source or any
 * animation using it is alive; write cache files to a temporary name and
 * rename them into place.
 * Returns WT_ERR_IO when the file can't be opened or mapped.
 * Thread: background thread (file I/O).
 */
WT_API wt_status wt_source_create_file(const char* path,
                                       const wt_limits* limits,
                                       wt_source** out_source);

/*
 * Releases the caller's reference to source. Animations created from it keep
 * it alive. NULL does nothing.
 * Thread: any thread, but not concurrently with another call that passes the
 * same handle.
 */
WT_API void wt_source_destroy(wt_source* source);

/*
 * Reads the container headers and frame index of source and fills *out_info.
 * Decodes no pixels. Returns WT_ERR_INVALID_DATA, WT_ERR_UNSUPPORTED,
 * WT_ERR_TRUNCATED or WT_ERR_LIMIT for input the core won't play; *out_info is
 * then zeroed.
 *
 * Every wt_limits field of the source is enforced here, so a file that probes
 * OK is within them: max_dimension (WT_ERR_LIMIT for a larger width or
 * height), max_frames, max_duration_ms, and max_total_pixels (width x height
 * x frame_count). max_bytes was checked when the source was created. The
 * frame vector is bounded by max_frames, so a hostile file can't make the
 * index grow past it.
 *
 * Probing the same source again returns the same result; wt_anim_create
 * applies the same checks.
 * Thread: background thread. Safe to call concurrently on the same source.
 */
WT_API wt_status wt_probe(wt_source* source, wt_info* out_info);

/* ------------------------------------------------------------------------- */
/* Animations                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Creates an animation that decodes source and scales every frame to exactly
 * width x height pixels (a size bucket). Pass 0 x 0 for the source size.
 * Otherwise both must be in 1..limits->max_dimension of the source's limits;
 * the caller picks a bucket with the source's aspect ratio.
 *
 * Takes its own reference to source (see Ownership). Probes and validates the
 * input, so it fails with the same errors as wt_probe. The cursor starts
 * before frame 0.
 * Thread: background thread.
 */
WT_API wt_status wt_anim_create(wt_source* source, uint32_t width,
                                uint32_t height, wt_anim** out_anim);

/*
 * Like wt_anim_create, backed by the frame cache file at cache_path (UTF-8):
 * one animation at one output size, in the format of docs/cache-format.md.
 * The caller names the file after the content and the size (PLAN.md:
 * "<hash>_<w>x<h>_v1.wsc") in a directory that exists.
 *
 * - A valid cache for this source (checked by size and CRC-32) at this size
 *   replaces decoding: frames are decompressed from it, never decoded.
 * - Otherwise the first pass that plays frame 0 to the last frame without a
 *   seek or an error writes the file as it goes, to a temporary name that is
 *   fsynced and renamed to cache_path after the last frame. Later loops of
 *   this animation read it.
 * - A file that fails validation, here or when a later frame is read, is
 *   deleted and playback decodes instead.
 * Cache problems (no file, a read-only directory, a full disk) never fail
 * this call or a frame; the animation just decodes. cache_path NULL or ""
 * behaves exactly like wt_anim_create.
 *
 * Several animations may use the same cache_path at once, in one process or
 * several: writers never collide and readers keep the file they mapped.
 * Thread: background thread (file I/O, CRC of the source).
 */
WT_API wt_status wt_anim_create_cached(wt_source* source, uint32_t width,
                                       uint32_t height, const char* cache_path,
                                       wt_anim** out_anim);

/*
 * Destroys anim and releases its reference to the source. NULL does nothing.
 * An unfinished cache file it was writing is deleted.
 * Thread: any thread, but not concurrently with another call on anim.
 */
WT_API void wt_anim_destroy(wt_anim* anim);

/*
 * Fills *out_info with the probe result for anim's source, and *out_width /
 * *out_height with the output size every frame is scaled to. Any out pointer
 * may be NULL.
 * Thread: any thread, but not concurrently with another call on anim.
 */
WT_API wt_status wt_anim_get_info(const wt_anim* anim, wt_info* out_info,
                                  uint32_t* out_width, uint32_t* out_height);

/*
 * Moves the cursor so the next wt_anim_next_frame returns frame_index.
 * Seeking decodes from the preceding random-access frame, so its cost grows
 * with the distance to it. wt_anim_seek(anim, 0) is cheap and is how a loop
 * restarts.
 * Returns WT_ERR_INVALID_ARGUMENT when frame_index >= frame_count.
 * Thread: background thread, not concurrently with another call on anim.
 */
WT_API wt_status wt_anim_seek(wt_anim* anim, uint32_t frame_index);

/*
 * Decodes the frame at the cursor, writes it into dst at the output size, and
 * advances the cursor.
 *
 * dst:          the first row of the destination, e.g. a locked
 *               ANativeWindow_Buffer or CVPixelBuffer.
 * dst_size:     bytes the core may write from dst. Must be at least
 *               stride * (height - 1) + width * 4.
 * stride:       bytes from the start of one row to the next; >= width * 4.
 * pixel_format: a WT_PIXEL_FORMAT_* value.
 * out_frame:    receives the frame's metadata; may be NULL.
 *
 * Returns WT_END, writing nothing, when the cursor is past the last frame.
 * Returns WT_ERR_INVALID_ARGUMENT for a buffer that is too small, without
 * decoding or advancing. Any other error still advances the cursor, so one
 * broken frame doesn't stall playback; dst may hold a partial frame, and the
 * caller should keep showing the previous one. A frame whose alpha payload
 * is missing or broken is not an error: it keeps the last good alpha until
 * the alpha stream's next keyframe.
 * With a cache (wt_anim_create_cached) this may also write, rename or delete
 * the cache file.
 * Thread: background thread, not concurrently with another call on anim.
 */
WT_API wt_status wt_anim_next_frame(wt_anim* anim, uint8_t* dst,
                                    size_t dst_size, uint32_t stride,
                                    wt_pixel_format pixel_format,
                                    wt_frame_info* out_frame);

/* ------------------------------------------------------------------------- */
/* Cache directory                                                            */
/* ------------------------------------------------------------------------- */

/*
 * The frame cache (docs/cache-format.md) lives in one directory per app. These
 * functions name its files and keep it under a disk budget
 * (docs/cache-format.md, "Budget and eviction"). They only ever touch what the
 * cache wrote: files named "*.wsc" and the writer's "*.wsc.*.tmp" temporaries,
 * directly inside the directory, never symlinks or anything else.
 *
 * A dir that doesn't exist holds nothing: stats and removal report zeros and
 * return WT_OK. Other failures to read it return WT_ERR_IO.
 */

/*
 * Sets the process-wide disk budget, in bytes: after each cache file is
 * written, the least recently used files in its directory are deleted until at
 * most this many bytes remain. Default 200 MiB; UINT64_MAX disables eviction;
 * 0 keeps nothing (every finished file is deleted at once, though the
 * animation that wrote it keeps playing from it).
 * Thread: any thread.
 */
WT_API void wt_cache_set_budget(uint64_t budget_bytes);

/*
 * Returns the budget set by wt_cache_set_budget.
 * Thread: any thread.
 */
WT_API uint64_t wt_cache_get_budget(void);

/*
 * Writes into out (out_size bytes, NUL-terminated) the path of the cache file
 * for the sticker identified by source_key at width x height inside dir:
 * "<dir>/<hash>_<width>x<height>_v1.wsc", where <hash> is 32 hex digits of the
 * SHA-256 of source_key. Pass the result to wt_anim_create_cached with the
 * animation's output size.
 * Returns WT_ERR_INVALID_ARGUMENT for a NULL or empty dir, a NULL key, a zero
 * size or a NULL or zero-sized out, and WT_ERR_LIMIT when out is too small (a
 * buffer of PATH_MAX bytes is enough for any path the OS accepts; out is
 * emptied).
 * Thread: any thread.
 */
WT_API wt_status wt_cache_file_path(const char* dir, const char* source_key,
                                    uint32_t width, uint32_t height, char* out,
                                    size_t out_size);

/*
 * Counts the cache files in dir.
 * Thread: background thread (reads the directory).
 */
WT_API wt_status wt_cache_get_stats(const char* dir, wt_cache_stats* out_stats);

/*
 * Deletes least recently used cache files from dir until it holds at most the
 * budget (wt_cache_set_budget), and writer temporaries left untouched for an
 * hour (a crash's leftovers). A cache hit counts as a use. *out_after (may be
 * NULL) receives what remains. The core also does this after each write; call
 * it once at startup, and again after lowering the budget.
 * Thread: background thread.
 */
WT_API wt_status wt_cache_evict(const char* dir, wt_cache_stats* out_after);

/*
 * Deletes every cache file and temporary in dir. *out_removed (may be NULL)
 * receives the cache files deleted. Animations that have a file open keep
 * playing from it; a writer whose temporary was deleted gives up quietly.
 * Thread: background thread.
 */
WT_API wt_status wt_cache_clear(const char* dir, wt_cache_stats* out_removed);

/* ------------------------------------------------------------------------- */
/* Scheduler                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * The scheduler plays every animation in the process on one clock thread and
 * a few worker threads (PLAN.md, "Scheduler"; ADR 0017). The platform glue
 * creates one wt_sched and turns each animation into a wt_player, giving it a
 * wt_sink: callbacks that hand out a destination buffer (a locked
 * ANativeWindow buffer, a pool CVPixelBuffer) and show it once drawn.
 *
 * - The clock runs at most options.max_fps rounds a second, and only while a
 *   player is due: with nothing playing it sleeps until something changes, so
 *   an idle scheduler costs no CPU.
 * - Each round queues the due frame of every playing player to the workers.
 *   A player has at most one frame in flight. Playback follows the content's
 *   own timing; when a frame is late (a slow worker, or content faster than
 *   max_fps) the frames in between are decoded but never shown, and after a
 *   long stall playback carries on from where it was instead of catching up.
 * - When more than options.max_concurrent players are active, the ones with
 *   the highest priority (wt_player_set_priority), then the most recently
 *   activated, play; the others hold their frame until a slot frees.
 * - Seeks and redraws are drawn as soon as a worker is free, whether or not
 *   the player is playing.
 */

/* Scheduler and player handles. Opaque. */
typedef struct wt_sched wt_sched;
typedef struct wt_player wt_player;

/* The most worker threads a scheduler runs. */
#define WT_SCHED_MAX_WORKERS 8

/*
 * Tunables of a scheduler; device tiers (PLAN.md, "Tiers") set them. Start
 * from wt_sched_options_init_default. Zero is invalid for worker_count and
 * max_fps.
 */
typedef struct wt_sched_options {
  /* Threads that decode frames, 1..WT_SCHED_MAX_WORKERS. Default: half the
     CPU cores, between 1 and 4. */
  uint32_t worker_count;
  /* Most clock rounds a second, 1..240, and so the most frames a second any
     player shows. Content faster than this skips frames to keep its timing.
     Default 60. */
  uint32_t max_fps;
  /* Most players that play at once; 0 means no limit. Default 0. */
  uint32_t max_concurrent;
} wt_sched_options;

/*
 * Optional hooks run by every scheduler thread (the clock and each worker)
 * when it starts and just before it exits; e.g. the Android glue attaches the
 * thread to the JVM for its lifetime. Either function may be NULL. name is
 * the thread's name ("wt-clock", "wt-worker-0", ...), static.
 */
typedef struct wt_sched_hooks {
  void* context;
  void (*thread_start)(void* context, const char* name);
  void (*thread_stop)(void* context);
} wt_sched_hooks;

/* What wt_sched_get_stats reports. */
typedef struct wt_sched_stats {
  /* Players alive. */
  uint32_t players;
  /* Players set active. */
  uint32_t active;
  /* Active players that are playing: within max_concurrent, with more than
     one frame, and not a non-looping player that ended. */
  uint32_t running;
  /* Worker threads running. */
  uint32_t workers;
  /* Clock rounds that queued at least one playback frame. */
  uint64_t ticks;
  /* Times the clock thread woke up, for whatever reason. */
  uint64_t wakeups;
  /* Frames drawn into a sink buffer successfully. */
  uint64_t frames_drawn;
  /* Playback frames decoded but never shown, to keep the content's timing. */
  uint64_t frames_skipped;
} wt_sched_stats;

/* A destination for one frame, filled by wt_sink.acquire. */
typedef struct wt_buffer {
  /* The first row. */
  uint8_t* pixels;
  /* Bytes the core may write from pixels; at least
     stride * (height - 1) + width * 4 for the player's output size. */
  size_t size;
  /* Bytes from one row to the next; >= width * 4. */
  uint32_t stride;
  /* A WT_PIXEL_FORMAT_* value. */
  wt_pixel_format format;
} wt_buffer;

/*
 * How a player gets its frames on screen. The scheduler calls these on its
 * worker threads, never two at once for the same player (one frame in flight),
 * and never after wt_player_destroy returns. They must not call wt_player_* or
 * wt_sched_* functions on their own scheduler.
 */
typedef struct wt_sink {
  void* context;
  /*
   * Provides a buffer of the player's output size for the next frame. Return
   * WT_OK after filling *out; release is then called exactly once, on the
   * same thread. Any other status skips this attempt, and nothing else is
   * called: WT_BUSY retries on the next tick, WT_NO_TARGET waits for
   * wt_player_redraw, and a negative status retries about 100 ms later.
   * Required.
   */
  wt_status (*acquire)(void* context, wt_buffer* out);
  /*
   * Ends the attempt that acquire started. status is WT_OK when the buffer
   * holds frame->index, ready to show; otherwise the buffer may hold a
   * partial frame and the previous frame should stay on screen. Required.
   */
  void (*release)(void* context, wt_status status, const wt_frame_info* frame);
  /*
   * A frame failed to decode (status < 0). Playback carries on with the next
   * one. Called for the first failure of a player only, so a broken file
   * can't flood the app. May be NULL.
   */
  void (*error)(void* context, wt_status status);
} wt_sink;

/*
 * Fills *out with the defaults listed in wt_sched_options. Does nothing when
 * out is NULL.
 * Thread: any thread.
 */
WT_API void wt_sched_options_init_default(wt_sched_options* out);

/*
 * Creates a scheduler and starts its threads (the clock and
 * options->worker_count workers, all idle until a player needs them).
 * options: NULL for the defaults. hooks: may be NULL; copied.
 * Returns WT_ERR_INVALID_ARGUMENT for an option out of range and
 * WT_ERR_NO_MEMORY when a thread can't be started.
 * Thread: any thread.
 */
WT_API wt_status wt_sched_create(const wt_sched_options* options,
                                 const wt_sched_hooks* hooks,
                                 wt_sched** out_sched);

/*
 * Stops the scheduler's threads, waiting for frames in flight, then destroys
 * every player still on it (their handles become invalid). NULL does nothing.
 * Thread: any thread but the scheduler's own (not from a sink callback).
 */
WT_API void wt_sched_destroy(wt_sched* sched);

/*
 * Applies new options to a running scheduler. Changes to max_fps and
 * max_concurrent take effect on the next round; fewer workers wait for their
 * frame in flight to finish.
 * Returns WT_ERR_INVALID_ARGUMENT for an option out of range (nothing
 * changes) and WT_ERR_NO_MEMORY when a new worker can't be started (the
 * others still apply; see wt_sched_get_stats).
 * Thread: any thread but the scheduler's own.
 */
WT_API wt_status wt_sched_set_options(wt_sched* sched,
                                      const wt_sched_options* options);

/*
 * Fills *out_stats with the scheduler's counters.
 * Thread: any thread.
 */
WT_API wt_status wt_sched_get_stats(wt_sched* sched, wt_sched_stats* out_stats);

/*
 * Adds anim to sched as a new player, inactive, with priority 0. It draws its
 * first frame as soon as the sink gives it a buffer, then holds it until
 * wt_player_set_active. With loop false, playback stops on the last frame and
 * holds it until wt_player_seek.
 *
 * On success the player owns anim (wt_player_destroy destroys it), and the
 * caller must not use anim again; on failure the caller keeps it. anim should
 * not have drawn frames yet: the player starts by seeking it to frame 0.
 * sink is copied; its acquire and release must be set.
 * Thread: any thread.
 */
WT_API wt_status wt_player_create(wt_sched* sched, wt_anim* anim, bool loop,
                                  const wt_sink* sink, wt_player** out_player);

/*
 * Removes player from its scheduler and destroys it and its animation. Waits
 * for a frame in flight, so no sink callback runs once this returns and the
 * sink's context may be freed. A player attached to an atlas leaves it first
 * (its slot is freed). NULL does nothing.
 * Thread: any thread but the scheduler's own (not from a sink callback), and
 * not concurrently with another call on player.
 */
WT_API void wt_player_destroy(wt_player* player);

/*
 * Starts or stops playback. A stopped player holds its frame and, when
 * started again, gives it the rest of its duration.
 * Thread: any thread; never blocks on a frame.
 */
WT_API void wt_player_set_active(wt_player* player, bool active);

/*
 * Ranks the player for options.max_concurrent: active players with a higher
 * priority play first, and among equals the most recently activated. The
 * glue passes the area, in physical pixels, that the player's widgets show
 * on screen.
 * Thread: any thread; never blocks on a frame.
 */
WT_API void wt_player_set_priority(wt_player* player, uint64_t priority);

/*
 * Shows frame_index (0-based) as soon as a worker is free, playing or not; a
 * playing player carries on from there, and a non-looping one that ended
 * plays again. Returns WT_ERR_INVALID_ARGUMENT when frame_index is past the
 * last frame.
 * Thread: any thread; never blocks on a frame.
 */
WT_API wt_status wt_player_seek(wt_player* player, uint32_t frame_index);

/*
 * The sink has a new target (say, a new surface): draws the frame that should
 * be showing into it, and lets a player that got WT_NO_TARGET draw again. A
 * playing player draws its next frame instead, which costs no seek.
 * Thread: any thread; never blocks on a frame.
 */
WT_API void wt_player_redraw(wt_player* player);

/* ------------------------------------------------------------------------- */
/* Atlas                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * An atlas shows many players through a few platform textures (PLAN.md,
 * "Two render modes"; ADR 0006, ADR 0020). It is a set of pages; each page is
 * one platform buffer queue (an ANativeWindow, a CVPixelBufferPool) that the
 * glue hands over as a wt_atlas_target. A player attached to the atlas draws
 * into a slot of a page (wt::SlotAllocator, ADR 0019) instead of its own
 * sink:
 *
 * - Workers decode each frame into a CPU copy of the slot, so every slot
 *   always has its latest frame at hand.
 * - When the jobs a scheduler round dispatched for a page's players are done,
 *   a worker presents the page: it locks one buffer of the target for the
 *   whole page, writes every live slot (content plus a cleared gutter, the
 *   slot's whole outer rect) and shows it. Queued buffers hold stale frames,
 *   so nothing is drawn incrementally. A page presents at most once per
 *   clock period (max_fps), and never twice at once. Its players show their
 *   playback frames on the page's beat (its shortest frame, at least a clock
 *   period; ADR 0021), so the page presents at the content rate however its
 *   players' clocks are phased.
 * - A page is as tall as its slots need (wanted_height in wt_atlas_page_info,
 *   from the rows in use). Its buffers have a fixed size, so the glue gives a
 *   page a new target (and a new platform texture) when wanted_height
 *   changes, and keeps the old texture on screen until Dart has switched to
 *   the new one. With no slots, a page wants no target at all.
 * - wt_atlas_detach (or destroying the atlas) gives a player back the sink it
 *   had before wt_atlas_attach, and redraws.
 *
 * Memory: the target's buffers (wt_atlas_page_info.buffers distinct buffers
 * of stride x height), plus one CPU frame per live slot and a few spare
 * frames for frames being decoded (cpu_bytes).
 *
 * Callbacks: the target's acquire and present run on scheduler workers,
 * never two at once for one page and never after wt_atlas_set_target (or
 * wt_atlas_destroy) has replaced the target. They must not call wt_atlas_*,
 * wt_player_* or wt_sched_* functions.
 */

/* Atlas handle. Opaque. */
typedef struct wt_atlas wt_atlas;

/* The most pages an atlas may have. */
#define WT_ATLAS_MAX_PAGES 16

/*
 * Layout of an atlas's pages. Start from wt_atlas_options_init_default.
 */
typedef struct wt_atlas_options {
  /* Page width in pixels, 16..2048, rounded down to a multiple of 16.
     Default 2048. */
  uint32_t width;
  /* The tallest a page may grow, 16..2048, rounded down to a multiple of 16.
     Default 2048. */
  uint32_t max_height;
  /* Transparent border on every side of a slot's content, 2..64. Default 2. */
  uint32_t gutter;
  /* Most pages, 1..WT_ATLAS_MAX_PAGES. Default 4. */
  uint32_t max_pages;
  /* WT_PIXEL_FORMAT_* of the pages' buffers. Default RGBA8888. */
  wt_pixel_format format;
} wt_atlas_options;

/* Where an attached player's frames go, in page pixels (origin top left). */
typedef struct wt_atlas_slot {
  /* Index of the page, 0..wt_atlas_page_count - 1. */
  uint32_t page;
  /* The content rect: the player's output size. */
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
  /* The whole slot: content plus a transparent gutter of at least `gutter`
     px on every side, 16-px aligned. */
  uint32_t outer_x;
  uint32_t outer_y;
  uint32_t outer_width;
  uint32_t outer_height;
} wt_atlas_slot;

/* What wt_atlas_get_page reports. */
typedef struct wt_atlas_page_info {
  /* Page width (options.width after rounding). */
  uint32_t width;
  /* Rows the slots use, a multiple of 16; 0 with no slots. */
  uint32_t used_height;
  /* The buffer height the page should have: used_height, or the current
     height while the rows freed are fewer than max(128, height / 4) (so
     pages don't flip-flop), or 0 with no slots (release the target). When
     it differs from `height`, give the page a new target of this height. */
  uint32_t wanted_height;
  /* The height last passed to wt_atlas_set_target. */
  uint32_t height;
  /* Whether the page has a target now. */
  uint32_t has_target;
  /* Slots in use. */
  uint32_t live_slots;
  /* Distinct buffers the current target handed out (by buffer id), and
     their row stride in bytes. */
  uint32_t buffers;
  uint32_t buffer_stride;
  /* buffers x buffer_stride x height: the target's buffer memory. */
  uint64_t buffer_bytes;
  /* CPU memory: the slots' latest frames and the spare frames for decoding. */
  uint64_t cpu_bytes;
  /* Presents shown, and presents that failed (no buffer, a bad buffer). */
  uint64_t presents;
  uint64_t present_failures;
} wt_atlas_page_info;

/*
 * A page's platform buffer queue. The core calls these on scheduler workers;
 * see "Callbacks" above.
 */
typedef struct wt_atlas_target {
  void* context;
  /*
   * Provides a buffer for the whole page: width x height (the height given
   * to wt_atlas_set_target) in options.format, rows top to bottom, its
   * contents undefined. *out_buffer_id identifies the underlying buffer (an
   * IOSurface ID, a gralloc address) for the memory stats; 0 uses
   * out->pixels instead. Return WT_OK after filling *out; present is then
   * called exactly once, on the same thread. WT_BUSY retries a period later,
   * WT_NO_TARGET waits for wt_atlas_set_target, and an error retries about
   * 100 ms later. Required.
   */
  wt_status (*acquire)(void* context, wt_buffer* out, uint64_t* out_buffer_id);
  /*
   * Ends what acquire started. With WT_OK the buffer holds the whole page:
   * show it. Otherwise the core rejected the buffer (wrong size or format)
   * and wrote nothing. Required.
   */
  void (*present)(void* context, wt_status status);
} wt_atlas_target;

/*
 * Fills *out with the defaults listed in wt_atlas_options. Does nothing when
 * out is NULL.
 * Thread: any thread.
 */
WT_API void wt_atlas_options_init_default(wt_atlas_options* out);

/*
 * Creates an empty atlas on sched (which must outlive it). Pages are created
 * as wt_atlas_attach needs them. options: NULL for the defaults.
 * Returns WT_ERR_INVALID_ARGUMENT for an option out of range.
 * Thread: any thread.
 */
WT_API wt_status wt_atlas_create(wt_sched* sched,
                                 const wt_atlas_options* options,
                                 wt_atlas** out_atlas);

/*
 * Gives every attached player back its own sink (as wt_atlas_detach), waits
 * for presents in flight, and frees the atlas. No target callback runs once
 * this returns, so the glue may release every page's buffers. NULL does
 * nothing.
 * Thread: any thread but the scheduler's own, not concurrently with another
 * call on atlas or on its players.
 */
WT_API void wt_atlas_destroy(wt_atlas* atlas);

/*
 * Moves player (on the atlas's scheduler) into a slot of its output size:
 * the lowest page with a free slot for that size, else the first page that
 * can grow or reuse rows for it, else a new page. Waits for the player's
 * frame in flight; from then on its frames go to the slot (its own sink's
 * error callback still reports decode failures) and the frame that should be
 * showing is drawn right away. A page that grew wants a new target (see
 * wt_atlas_page_info.wanted_height); until it has one, the new slot isn't
 * shown.
 *
 * Returns WT_ERR_INVALID_ARGUMENT for a player already in an atlas, on
 * another scheduler, or whose size can never fit a page, and WT_ERR_LIMIT
 * when every page is full and there are max_pages of them (nothing changes;
 * keep the player standalone).
 * Thread: any thread but the scheduler's own, not concurrently with another
 * call on atlas or player.
 */
WT_API wt_status wt_atlas_attach(wt_atlas* atlas, wt_player* player,
                                 wt_atlas_slot* out_slot);

/*
 * Frees player's slot and gives it back the sink it had before
 * wt_atlas_attach, waiting for its frame in flight; the frame that should be
 * showing is then drawn into that sink. The page may now want a smaller
 * target, or none. Returns WT_ERR_INVALID_ARGUMENT when player isn't in this
 * atlas.
 * Thread: as wt_atlas_attach.
 */
WT_API wt_status wt_atlas_detach(wt_atlas* atlas, wt_player* player);

/*
 * Pages created so far; a page's index never changes. 0 for NULL.
 * Thread: any thread, not concurrently with wt_atlas_destroy.
 */
WT_API uint32_t wt_atlas_page_count(wt_atlas* atlas);

/*
 * Fills *out_info for page index `page` (< wt_atlas_page_count).
 * Thread: any thread, not concurrently with wt_atlas_destroy. Never waits for
 * a frame or a present.
 */
WT_API wt_status wt_atlas_get_page(wt_atlas* atlas, uint32_t page,
                                   wt_atlas_page_info* out_info);

/*
 * Sets the buffer queue page `page` presents into, waiting for a present in
 * flight on the old one: once this returns, the old target's callbacks are
 * never called again. target is copied; NULL leaves the page without one
 * (say, while the surface is gone). height is the target's buffer height, a
 * multiple of 16 in 16..options.max_height (normally wanted_height); with a
 * NULL target it is the height the page will have when a target comes back,
 * or 0 when the page has no buffers any more. A new target is presented into
 * right away with every live slot that fits in its height.
 * Returns WT_ERR_INVALID_ARGUMENT for an unknown page, a bad height, or a
 * target without acquire or present.
 * Thread: any thread but the scheduler's own, not concurrently with another
 * call on atlas.
 */
WT_API wt_status wt_atlas_set_target(wt_atlas* atlas, uint32_t page,
                                     const wt_atlas_target* target,
                                     uint32_t height);

/*
 * The page whose current target shows player's slot: its index once a
 * present into the target last given by wt_atlas_set_target has shown a
 * frame of player in the slot, else -1. It is -1 before the slot's first
 * frame, after a new target until a present into it, and for a player that
 * isn't in atlas (or NULL arguments). Callers keep showing what they showed
 * before (a placeholder, an older texture) until then: until a present
 * wrote it, a slot's area of the target's buffers holds stale pixels.
 * Thread: as wt_atlas_attach (may wait while another thread attaches);
 * never waits for a frame or a present.
 */
WT_API int32_t wt_atlas_shown_page(wt_atlas* atlas, const wt_player* player);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WT_API_H_ */
