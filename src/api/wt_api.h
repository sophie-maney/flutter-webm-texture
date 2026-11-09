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
 * Functions that later tasks implement return WT_ERR_UNIMPLEMENTED after
 * checking their arguments. The frame cache, scheduler and atlas (Phases 3-4)
 * will add functions here; they won't change the existing ones without bumping
 * WT_API_VERSION.
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
#define WT_API_VERSION 1

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
  /* The video has an alpha channel (WebM AlphaMode = 1, a WebP alpha flag or
     a GIF transparent index). False means every pixel is opaque. */
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
  int64_t pts_ms;       /* Presentation time from the start of the stream. */
} wt_frame_info;

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
 * Destroys anim and releases its reference to the source. NULL does nothing.
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
 * On an error dst may hold a partial frame; the caller should keep showing
 * the previous one.
 * Thread: background thread, not concurrently with another call on anim.
 */
WT_API wt_status wt_anim_next_frame(wt_anim* anim, uint8_t* dst,
                                    size_t dst_size, uint32_t stride,
                                    wt_pixel_format pixel_format,
                                    wt_frame_info* out_frame);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WT_API_H_ */
