// WebM demuxer: a minimal EBML reader that indexes the video track
// (ADR 0004).
//
// It reads only what sticker playback needs: the EBML header, the first
// Segment, Info (TimecodeScale), Tracks (the first VP8/VP9 video track, and
// whether any audio track exists) and Clusters (Timecode, SimpleBlock, and
// BlockGroup with Block, BlockAdditions, BlockDuration and ReferenceBlock).
// Everything else is skipped by size.
//
// Sticker files come from other users, so the reader is written for hostile
// input:
//   - Payloads are recorded as byte spans into the input and never copied.
//     The only allocation is the frame vector, capped at limits.max_frames.
//   - No recursion. Each nesting level has its own loop, and every iteration
//     advances by at least one element header, so parsing is linear.
//   - Every element must fit inside its parent. If the parent ends at EOF
//     (unknown size, or a size that runs past EOF), a child that runs past it
//     means truncation; otherwise the input is malformed.
//   - Unknown sizes are accepted only for Segment and Cluster, as live muxers
//     (MediaRecorder, ffmpeg to a pipe) write them. An unknown-size Cluster
//     ends at the first element that can't be a Cluster child.
//   - Lacing and ContentEncodings on the video track are unsupported.
//
// Limits (T1.3, T1.5): max_dimension and max_frames bound the index itself and
// max_total_pixels is checked as each frame is added, so a hostile file is
// refused before it costs more than max_frames entries. max_duration_ms needs
// every frame's duration, so it is checked once the whole file is indexed.
#ifndef WT_DEMUX_WEBM_H_
#define WT_DEMUX_WEBM_H_

#include <cstdint>
#include <vector>

#include "api/wt_api.h"
#include "demux/source.h"

namespace wt {

// A byte range inside the demuxed input. Empty (size 0) means "absent".
struct Span {
  uint64_t offset = 0;
  uint64_t size = 0;
  bool empty() const { return size == 0; }
};

struct WebmVideoTrack {
  uint64_t number = 0;
  wt_codec codec = WT_CODEC_UNKNOWN;  // WT_CODEC_VP8 or WT_CODEC_VP9.
  uint32_t width = 0;                 // PixelWidth.
  uint32_t height = 0;                // PixelHeight.
  // Matroska AlphaMode. Only 1 means the frames carry alpha, in a
  // BlockAdditional with BlockAddID 1; any other value means opaque.
  uint64_t alpha_mode = 0;
  // DefaultDuration in ns, or 0 when absent (or too large for int64).
  int64_t default_duration_ns = 0;

  bool has_alpha() const { return alpha_mode == 1; }
};

struct WebmFrame {
  Span data;   // The VP8/VP9 frame (or VP9 superframe).
  Span alpha;  // BlockAdditional with BlockAddID 1. Empty when the frame has
               // none or the track's AlphaMode isn't 1.
  // (Cluster Timecode + block offset) x TimecodeScale. May be negative.
  int64_t pts_ns = 0;
  // Always > 0. The next frame's pts minus this one's; when that isn't
  // positive, or for the last frame, the block's BlockDuration, then the
  // track's DefaultDuration, then 1/30 s.
  int64_t duration_ns = 0;
  // Container keyframe flag: SimpleBlock bit 0x80, or a BlockGroup without
  // ReferenceBlock. Informational only; seek with random_access.
  bool keyframe = false;
  // The bitstream says decoding can start here: the colour payload is a
  // keyframe and so is the alpha payload, if there is one (spike A,
  // pitfall 1). Colour and alpha come from independent encoders, so this can
  // be false on a container keyframe.
  bool random_access = false;
};

struct WebmIndex {
  // WT_OK: parsed to the end of the Segment.
  // WT_ERR_TRUNCATED: the input ends inside an element. `frames` holds every
  //   frame whose whole block was in bounds (possibly none).
  // Any other error: `frames` is empty; the other fields say how far parsing
  //   got and are for diagnostics only.
  wt_status status = WT_OK;
  // A static, human-readable reason when status != WT_OK. Never null.
  const char* error = "";
  uint64_t timecode_scale_ns = 1000000;
  bool has_video = false;  // A VP8/VP9 video track was found.
  bool has_audio = false;  // Any audio track was listed. It is ignored.
  WebmVideoTrack video;
  std::vector<WebmFrame> frames;
};

// Indexes the WebM in `bytes` into *out (which is reset first) and returns
// out->status. Every wt_limits field the demuxer reads must be non-zero
// (max_dimension, max_frames, max_total_pixels, max_duration_ms), or the
// result is WT_ERR_INVALID_ARGUMENT.
//
// Besides the statuses above: WT_ERR_INVALID_DATA for malformed input or a
// video track with no frames, WT_ERR_UNSUPPORTED for a well-formed file
// outside what we play (no VP8/VP9 track, lacing, ContentEncodings), and
// WT_ERR_LIMIT when the size, frame count, decode budget (width x height x
// frames) or total duration exceeds limits. A limit error wins over
// truncation and leaves `frames` empty.
//
// Never reads outside `bytes`. Every span in the result lies inside it.
// Thread: background thread (linear in the input size). Reentrant.
wt_status DemuxWebm(ByteView bytes, const wt_limits& limits, WebmIndex* out);

}  // namespace wt

#endif  // WT_DEMUX_WEBM_H_
