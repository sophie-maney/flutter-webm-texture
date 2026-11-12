// wt::Anim: the playback cursor behind a wt_anim handle.
//
// Scope: WebM (VP8/VP9, with or without alpha), animated WebP and GIF, the
// last two through the CanvasDecoder seam. Limits (wt_limits) are enforced
// when the file is indexed (ProbeSource) and, as a guard, while decoding.
//
// Per frame, decoding: decode at source size (VpxDecoder, or a CanvasDecoder
// whose RGBA canvas RgbaToYuva turns into full-range BT.601 YUVA) -> scale
// into a YUVA 4:2:0 frame at the output size (YuvaImage) -> premultiplied
// RGBA/BGRA in the caller's buffer. A canvas decoder is sequential, so a
// seek backwards resets it and decodes forward from frame 0 (within the
// decode budget). The scaled frame persists between calls, which is
// how a packet that shows nothing repeats the previous frame and how a frame
// with a broken alpha payload keeps the last good alpha.
//
// Frame cache (T3.1, docs/cache-format.md): with a cache path, Create first
// tries to read the .wsc file there. A valid one replaces the decoder
// entirely: each frame is LZ4-decompressed straight into the YuvaImage. With
// none, the first pass that plays from frame 0 to the end without a seek or
// an error streams every frame into a WscWriter, which renames the file into
// place after the last frame; playback then switches to reading it and
// frees the decoder. A file that fails validation, at open or on any later
// frame, is deleted and playback carries on by decoding (the next full pass
// writes a fresh one). Cache I/O problems never fail a frame.
//
// Thread: background thread. Not thread-safe (see wt_api.h, Threading).
#ifndef WT_API_ANIM_H_
#define WT_API_ANIM_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/wt_api.h"
#include "cache/wsc.h"
#include "convert/yuva.h"
#include "decode/canvas_decoder.h"
#include "decode/vpx_decoder.h"
#include "demux/gif.h"
#include "demux/source.h"
#include "demux/webm.h"
#include "demux/webp.h"

namespace wt {

// What every format reports per frame, which is all playback needs from the
// index: how long it shows, when it starts (ms, relative to frame 0), and
// whether decoding can start there from scratch.
struct FrameTiming {
  uint32_t duration_ms = 1;  // >= 1.
  int64_t pts_ms = 0;
  bool random_access = false;
};

// The index of a probed source: format-neutral timing for every playable
// frame, plus the format's own index for decoding.
struct SourceIndex {
  std::vector<FrameTiming> frames;
  WebmIndex webm;  // WT_FORMAT_WEBM only.
  WebpIndex webp;  // WT_FORMAT_WEBP only.
  GifIndex gif;    // WT_FORMAT_GIF only.
};

// Fills *out from a demuxed WebM whose status is WT_OK or (with frames)
// WT_ERR_TRUNCATED. Durations saturate rather than overflow.
void WebmInfo(const WebmIndex& index, wt_info* out);

// Reads the headers and frame index of `bytes` (sniffed as `format`) and
// fills *out_info, enforcing every wt_limits field except max_bytes, which
// the source checked on creation. This is wt_probe, and wt_anim_create runs it
// too, so both fail identically. *out_info is zeroed on failure.
//
// Errors: WT_ERR_INVALID_DATA for an unknown format or malformed input,
// WT_ERR_UNSUPPORTED for files the core won't play,
// WT_ERR_TRUNCATED when no complete frame exists, WT_ERR_LIMIT. A file cut
// after its first frame succeeds with out_info->truncated set. If `index` is
// non-null it receives the frame index on success. A WebP or GIF is a canvas
// of `width` x `height` shown frame after frame; a still WebP is one frame.
// See demux/webp.h and demux/gif.h for the duration policies. Thread:
// background thread.
wt_status ProbeSource(ByteView bytes, wt_format format, const wt_limits& limits,
                      wt_info* out_info, SourceIndex* index);

class Anim {
 public:
  // How frames are being produced, for tests and diagnostics.
  enum class CacheMode {
    kNone,     // Decoding, with no cache path (or caching gave up).
    kWriting,  // Decoding, and writing the cache as frames go by.
    kPending,  // Decoding; the next pass from frame 0 starts writing.
    kReading,  // Reading frames from the cache file.
  };

  // Demuxes `bytes` (which must outlive the Anim) and prepares to decode it
  // at width x height, or at source size for 0 x 0. A non-zero size must be
  // in 1..limits.max_dimension on both axes (WT_ERR_INVALID_ARGUMENT).
  // Fails like ProbeSource for unplayable input. cache_path (may be null)
  // names the .wsc file to read or write; a missing, stale or damaged file
  // is not an error.
  static wt_status Create(ByteView bytes, wt_format format,
                          const wt_limits& limits, uint32_t width,
                          uint32_t height, const char* cache_path,
                          std::unique_ptr<Anim>* out);

  ~Anim();
  Anim(const Anim&) = delete;
  Anim& operator=(const Anim&) = delete;

  const wt_info& info() const { return info_; }
  // How long frame `index` (< frame_count) is shown, in ms; at least 1.
  uint32_t frame_duration_ms(uint32_t index) const {
    return FrameDurationMs(index);
  }
  CacheMode cache_mode() const;
  uint32_t width() const { return frame_.width(); }
  uint32_t height() const { return frame_.height(); }

  // See wt_anim_seek. Cheap: the decoding happens in the next NextFrame.
  wt_status Seek(uint32_t frame_index);

  // See wt_anim_next_frame. The cursor advances on errors too, so a broken
  // frame doesn't stall playback. An invalid buffer is rejected before any
  // decoding and doesn't advance it.
  wt_status NextFrame(uint8_t* dst, size_t dst_size, uint32_t stride,
                      wt_pixel_format format, wt_frame_info* out_frame);

  // Moves the cursor past one frame without writing pixels: what NextFrame
  // does minus the conversion into a buffer. While decoding, the frame is
  // still decoded (later frames depend on it) and streamed to the cache
  // writer, so a pass with skipped frames still writes the cache; read from
  // the cache, a skip costs nothing. Returns WT_END after the last frame,
  // and like NextFrame advances the cursor on errors. The scheduler skips
  // frames that are already late (sched/timeline.h).
  wt_status SkipFrame(wt_frame_info* out_frame);

 private:
  Anim() = default;

  // The last random-access frame at or before index, or 0 when none is.
  uint32_t RandomAccessBefore(uint32_t index) const;
  // Feeds frame `index` to the decoder; when present, also scales its output
  // into frame_.
  wt_status DecodeFrame(uint32_t index, bool present);
  // Canvas formats: decodes the next canvas and, when present, converts it
  // (and scales it) into frame_.
  wt_status DecodeCanvasFrame(bool present);
  // DecodeAt for a canvas decoder: seeks it to `index` by resetting and
  // decoding forward.
  wt_status CatchUpCanvas(uint32_t index);
  bool has_decoder() const { return decoder_ != nullptr || canvas_ != nullptr; }
  // Decodes frame `index` into frame_, catching up from a seek first, and
  // streams it to the cache writer when one is open.
  wt_status DecodeAt(uint32_t index);
  // Takes the frame at the cursor and advances it: reads it from the cache
  // into frame_ when need_pixels (otherwise a cache hit reads nothing), or
  // decodes it (DecodeAt). Fills *out_frame on success when non-null.
  wt_status Advance(bool need_pixels, wt_frame_info* out_frame);
  // Creates the decoder if there is none (the cache made it unnecessary so
  // far), ready to decode up to frame `target`.
  wt_status EnsureDecoder(uint32_t target);
  // The timing of frame `index` as wt_frame_info reports it.
  uint32_t FrameDurationMs(uint32_t index) const;
  int64_t FramePtsMs(uint32_t index) const;

  // Cache plumbing; see the file comment.
  WscParams CacheParams() const;
  // Opens cache_path_ for reading; deletes a file that is there but unusable.
  void OpenCache();
  // Deletes the cache file and stops reading it.
  void DropCache();
  // Passes frame `index`, now in frame_, to the writer (starting one at
  // frame 0); finishes the file after the last frame and switches to it.
  void WriteCache(uint32_t index);
  // Stops caching for the rest of this Anim's life.
  void DisableCache();

  ByteView bytes_;
  SourceIndex index_;
  wt_info info_{};
  wt_limits limits_{};
  // Exactly one of these exists while decoding (neither while the cache
  // serves): decoder_ for WebM, canvas_ for the canvas formats (WebP, GIF).
  std::unique_ptr<VpxDecoder> decoder_;
  std::unique_ptr<CanvasDecoder> canvas_;
  // Canvas formats (WebP, GIF) at a scaled output: the source-size YUVA that
  // RgbaToYuva writes before ScaleColor/ScaleAlpha shrink it into frame_.
  YuvaImage stage_;
  YuvaImage frame_;          // The current frame at the output size.
  bool have_frame_ = false;  // frame_ holds a decoded frame.
  // The last packet decoded, so the decoder can continue from next_decode_
  // rather than waiting for a keyframe.
  bool decoder_healthy_ = true;
  // Decode budget of the current pass: source-size pixels decoded since the
  // last Seek (or creation). ProbeSource already bounds a full pass; this
  // catches a decoder that does more work than the index promised.
  uint64_t pass_pixels_ = 0;
  uint64_t max_pass_pixels_ = 0;
  uint32_t cursor_ = 0;       // Next frame NextFrame returns.
  uint32_t next_decode_ = 0;  // Next frame the decoder expects.

  std::string cache_path_;  // Empty: no caching.
  uint32_t source_crc_ = 0;
  std::unique_ptr<WscReader> reader_;
  std::unique_ptr<WscWriter> writer_;
};

}  // namespace wt

#endif  // WT_API_ANIM_H_
