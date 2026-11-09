// Spike A (T0.7): the frame index both prototype demuxers produce.
//
// This mirrors what T1.3 is asked to output: the video track plus, per frame,
// byte spans into the original file for the colour payload and the
// BlockAdditional (BlockAddID 1) alpha payload. Nothing is copied; the
// decoder reads the spans straight out of the (memory-mapped) input.
#ifndef SPIKES_ALPHA_FRAME_INDEX_H_
#define SPIKES_ALPHA_FRAME_INDEX_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spike {

enum class Codec { kUnknown, kVp8, kVp9 };

enum class IndexStatus {
  kOk,           // Parsed to the end.
  kTruncated,    // Input ended inside an element; frames so far are usable.
  kInvalid,      // Malformed input; frames so far may still be usable.
  kUnsupported,  // Well-formed but outside what we play (lacing, no video...).
  kLimit,        // A limit was exceeded.
};

const char* ToString(IndexStatus s);

struct Span {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  bool present() const { return size != 0; }
  bool operator==(const Span& o) const {
    return offset == o.offset && size == o.size;
  }
};

struct VideoTrack {
  std::uint64_t number = 0;
  Codec codec = Codec::kUnknown;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t alpha_mode = 0;  // Matroska AlphaMode; 1 = alpha track.
  std::uint64_t default_duration_ns = 0;
};

struct FrameEntry {
  Span data;   // Colour payload (the VP8/VP9 frame or superframe).
  Span alpha;  // BlockAdditional with BlockAddID 1, if any.
  std::int64_t pts_ns = 0;
  bool keyframe =
      false;  // Container flag (SimpleBlock bit / no ReferenceBlock)
  bool operator==(const FrameEntry& o) const {
    return data == o.data && alpha == o.alpha && pts_ns == o.pts_ns &&
           keyframe == o.keyframe;
  }
};

struct FrameIndex {
  IndexStatus status = IndexStatus::kOk;
  std::string error;  // Human-readable reason when status != kOk.
  std::uint64_t timecode_scale_ns = 1000000;
  bool has_video = false;
  bool has_audio = false;
  VideoTrack video;
  std::vector<FrameEntry> frames;
};

struct Limits {
  std::uint32_t max_dimension = 1024;
  std::size_t max_frames = 1000;
  // Biggest single leaf element we are willing to touch (CodecPrivate, a
  // BlockAdditional, ...). Frames are never copied, so this only bounds what
  // a parser might allocate.
  std::uint64_t max_leaf_bytes = 2u << 20;
};

// Set by `alpha_spike experiments` to show what webm_parser does without the
// caps our callback adds (see demux_libwebm.cc).
extern bool g_libwebm_unguarded;

// Both demuxers index an in-memory file. They never read past `size`.
FrameIndex IndexWithLibwebm(const std::uint8_t* data, std::size_t size,
                            const Limits& limits);
FrameIndex IndexWithMiniEbml(const std::uint8_t* data, std::size_t size,
                             const Limits& limits);

}  // namespace spike

#endif  // SPIKES_ALPHA_FRAME_INDEX_H_
