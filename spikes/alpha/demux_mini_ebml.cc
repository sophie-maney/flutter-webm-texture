// Spike A (T0.7): a minimal EBML/WebM reader, as the alternative to
// webm_parser.
//
// It understands only what sticker playback needs: the EBML header, Segment,
// Info (TimecodeScale), Tracks (number, type, codec, size, AlphaMode,
// DefaultDuration, ContentEncodings), Cluster (Timecode, SimpleBlock,
// BlockGroup with Block, BlockAdditions and ReferenceBlock). Everything else
// is skipped by size.
//
// Design rules:
//   - No allocation apart from the output frame vector (bounded by
//     max_frames). Payloads are recorded as spans and never copied.
//   - No recursion. Each nesting level has its own loop, and every loop
//     advances by at least one header (>= 2 bytes), so it terminates.
//   - Every element must fit inside its parent. If the parent's end is the
//     end of the file (unknown size, or a size that runs past EOF), a child
//     that runs past it means truncation; otherwise it is malformed input.
//   - Unknown sizes are accepted only for Segment and Cluster, as live
//     muxers (MediaRecorder, ffmpeg to a pipe) write them. An unknown-size
//     Cluster ends at the first element that can't be a Cluster child.
#include <cstring>

#include "frame_index.h"

namespace spike {
namespace {

enum : std::uint32_t {
  kIdEbml = 0x1A45DFA3,
  kIdDocType = 0x4282,
  kIdSegment = 0x18538067,
  kIdInfo = 0x1549A966,
  kIdTimecodeScale = 0x2AD7B1,
  kIdTracks = 0x1654AE6B,
  kIdTrackEntry = 0xAE,
  kIdTrackNumber = 0xD7,
  kIdTrackType = 0x83,
  kIdCodecId = 0x86,
  kIdDefaultDuration = 0x23E383,
  kIdContentEncodings = 0x6D80,
  kIdVideo = 0xE0,
  kIdPixelWidth = 0xB0,
  kIdPixelHeight = 0xBA,
  kIdAlphaMode = 0x53C0,
  kIdCluster = 0x1F43B675,
  kIdTimecode = 0xE7,
  kIdPosition = 0xA7,
  kIdPrevSize = 0xAB,
  kIdSilentTracks = 0x5854,
  kIdEncryptedBlock = 0xAF,
  kIdSimpleBlock = 0xA3,
  kIdBlockGroup = 0xA0,
  kIdBlock = 0xA1,
  kIdBlockAdditions = 0x75A1,
  kIdBlockMore = 0xA6,
  kIdBlockAddId = 0xEE,
  kIdBlockAdditional = 0xA5,
  kIdReferenceBlock = 0xFB,
  kIdVoid = 0xEC,
  kIdCrc32 = 0xBF,
};

constexpr std::uint64_t kTrackTypeVideo = 1;
constexpr std::uint64_t kTrackTypeAudio = 2;
constexpr std::size_t kMaxBlockMores = 8;

enum class R { kOk, kTruncated, kInvalid, kUnsupported, kLimit };

struct Elem {
  std::uint32_t id = 0;
  std::uint64_t pos = 0;   // First header byte.
  std::uint64_t body = 0;  // First body byte.
  std::uint64_t end = 0;   // One past the body, clamped to the parent.
  bool unknown = false;    // Size was "unknown" (all ones).
  bool open_end = false;   // `end` is the end of the file, not a real size.
  bool truncated = false;  // Segment/Cluster whose size runs past EOF.
};

class Parser {
 public:
  Parser(const std::uint8_t* d, std::size_t n, const Limits& l, FrameIndex* o)
      : d_(d), n_(n), limits_(l), out_(o) {}

  R Run() {
    Elem file;
    file.end = n_;
    file.open_end = true;
    std::uint64_t pos = 0;

    Elem ebml;
    R r = Header(pos, file, &ebml);
    if (r != R::kOk) return r;
    if (ebml.id != kIdEbml || ebml.unknown)
      return Fail(R::kInvalid, "no EBML header");
    r = ParseEbmlHeader(ebml);
    if (r != R::kOk) return r;
    pos = ebml.end;

    // Only the first Segment is read; anything after it is ignored.
    while (pos < n_) {
      Elem e;
      r = Header(pos, file, &e);
      if (r != R::kOk) return r;
      if (e.id == kIdSegment) return ParseSegment(e);
      if (e.unknown) return Fail(R::kInvalid, "unknown-size top-level element");
      pos = e.end;
    }
    return Fail(R::kInvalid, "no Segment");
  }

  const char* why() const { return why_; }

 private:
  R Fail(R r, const char* why) {
    why_ = why;
    return r;
  }

  // Reads the element header at `pos` inside `parent`. On kOk, `e` is filled
  // in and fits inside the parent (clamped and marked open_end if the parent
  // is open-ended).
  R Header(std::uint64_t pos, const Elem& parent, Elem* e) {
    e->pos = pos;
    if (pos >= n_) return Fail(R::kTruncated, "EOF before element header");
    // ID: 1-4 bytes, length from the leading zeros, marker bit kept.
    const std::uint8_t b0 = d_[pos];
    int id_len = 1;
    while (id_len <= 4 && !(b0 & (0x80 >> (id_len - 1)))) ++id_len;
    if (id_len > 4) return Fail(R::kInvalid, "bad element ID");
    if (pos + id_len > parent.end) {
      return parent.open_end ? Fail(R::kTruncated, "EOF inside element ID")
                             : Fail(R::kInvalid, "element ID overflows parent");
    }
    std::uint32_t id = 0;
    for (int i = 0; i < id_len; ++i) id = (id << 8) | d_[pos + i];
    pos += id_len;

    // Size: 1-8 bytes. All value bits set means "unknown".
    if (pos >= parent.end) {
      return parent.open_end
                 ? Fail(R::kTruncated, "EOF before element size")
                 : Fail(R::kInvalid, "element size overflows parent");
    }
    const std::uint8_t s0 = d_[pos];
    int size_len = 1;
    while (size_len <= 8 && !(s0 & (0x80 >> (size_len - 1)))) ++size_len;
    if (size_len > 8) return Fail(R::kInvalid, "bad element size");
    if (pos + size_len > parent.end) {
      return parent.open_end
                 ? Fail(R::kTruncated, "EOF inside element size")
                 : Fail(R::kInvalid, "element size overflows parent");
    }
    std::uint64_t size = s0 & (0xFF >> size_len);
    bool all_ones = size == (0xFFu >> size_len);
    for (int i = 1; i < size_len; ++i) {
      size = (size << 8) | d_[pos + i];
      all_ones = all_ones && d_[pos + i] == 0xFF;
    }
    pos += size_len;

    e->id = id;
    e->body = pos;
    e->unknown = all_ones;
    e->truncated = false;
    if (all_ones) {
      // Callers accept this only for Segment and Cluster. It ends with its
      // parent, or earlier at the first element that can't be its child.
      e->end = parent.end;
      e->open_end = parent.open_end;
      return R::kOk;
    }
    // `size` fits in 56 bits and `pos` <= n_, so this can't overflow.
    const std::uint64_t end = pos + size;
    if (end > parent.end) {
      if (!parent.open_end)
        return Fail(R::kInvalid, "element overflows parent");
      // Runs past EOF. Segment and Cluster are read up to EOF so the frames
      // before the cut survive; anything else is unusable.
      if (id != kIdSegment && id != kIdCluster) {
        return Fail(R::kTruncated, "EOF inside element");
      }
      e->end = parent.end;
      e->open_end = true;
      e->truncated = true;
      return R::kOk;
    }
    e->end = end;
    e->open_end = false;
    return R::kOk;
  }

  R Uint(const Elem& e, std::uint64_t* v) {
    if (e.unknown) return Fail(R::kInvalid, "unknown-size integer");
    const std::uint64_t len = e.end - e.body;
    if (len > 8) return Fail(R::kInvalid, "integer longer than 8 bytes");
    std::uint64_t x = 0;
    for (std::uint64_t i = 0; i < len; ++i) x = (x << 8) | d_[e.body + i];
    *v = x;
    return R::kOk;
  }

  bool StrEq(const Elem& e, const char* s) const {
    const std::size_t len = std::strlen(s);
    std::uint64_t n = e.end - e.body;
    // Strings may be NUL padded.
    while (n > 0 && d_[e.body + n - 1] == 0) --n;
    return n == len && std::memcmp(d_ + e.body, s, len) == 0;
  }

  // Leaves we read into values must be complete and known-size.
  R Leaf(const Elem& e) {
    if (e.unknown) return Fail(R::kInvalid, "unknown-size leaf element");
    if (e.end - e.body > limits_.max_leaf_bytes) {
      return Fail(R::kLimit, "leaf element larger than max_leaf_bytes");
    }
    return R::kOk;
  }

  R ParseEbmlHeader(const Elem& ebml) {
    R r = Leaf(ebml);  // The header is small and must be complete.
    if (r != R::kOk) return r;
    std::uint64_t pos = ebml.body;
    bool doctype_ok = false;
    while (pos < ebml.end) {
      Elem e;
      if ((r = Header(pos, ebml, &e)) != R::kOk) return r;
      if (e.unknown) return Fail(R::kInvalid, "unknown-size EBML child");
      if (e.id == kIdDocType) {
        doctype_ok = StrEq(e, "webm") || StrEq(e, "matroska");
      }
      pos = e.end;
    }
    return doctype_ok ? R::kOk : Fail(R::kInvalid, "DocType is not webm");
  }

  R ParseSegment(const Elem& seg) {
    std::uint64_t pos = seg.body;
    while (pos < seg.end) {
      Elem e;
      R r = Header(pos, seg, &e);
      if (r != R::kOk) return r;
      if (e.unknown && e.id != kIdCluster) {
        return Fail(R::kInvalid, "unknown-size element other than Cluster");
      }
      switch (e.id) {
        case kIdInfo:
          r = ParseInfo(e);
          break;
        case kIdTracks:
          r = ParseTracks(e);
          break;
        case kIdCluster:
          r = ParseCluster(e, &pos);
          if (r != R::kOk) return r;
          continue;  // ParseCluster set pos.
        default:
          break;  // SeekHead, Cues, Tags, Void, ... skipped by size.
      }
      if (r != R::kOk) return r;
      pos = e.end;
    }
    // A Segment whose size runs past EOF is a truncated file, even if the
    // last element happened to end exactly at EOF.
    if (seg.truncated) return Fail(R::kTruncated, "Segment runs past EOF");
    return R::kOk;
  }

  R ParseInfo(const Elem& info) {
    std::uint64_t pos = info.body;
    while (pos < info.end) {
      Elem e;
      R r = Header(pos, info, &e);
      if (r != R::kOk) return r;
      if (e.unknown) return Fail(R::kInvalid, "unknown-size Info child");
      if (e.id == kIdTimecodeScale) {
        std::uint64_t v = 0;
        if ((r = Uint(e, &v)) != R::kOk) return r;
        if (v == 0) return Fail(R::kInvalid, "TimecodeScale is 0");
        out_->timecode_scale_ns = v;
      }
      pos = e.end;
    }
    return R::kOk;
  }

  R ParseTracks(const Elem& tracks) {
    std::uint64_t pos = tracks.body;
    while (pos < tracks.end) {
      Elem e;
      R r = Header(pos, tracks, &e);
      if (r != R::kOk) return r;
      if (e.unknown) return Fail(R::kInvalid, "unknown-size Tracks child");
      if (e.id == kIdTrackEntry && (r = ParseTrackEntry(e)) != R::kOk) return r;
      pos = e.end;
    }
    return R::kOk;
  }

  R ParseTrackEntry(const Elem& te) {
    std::uint64_t number = 0, type = 0, default_duration = 0;
    std::uint64_t width = 0, height = 0, alpha_mode = 0;
    bool has_video_elem = false, encoded = false;
    Codec codec = Codec::kUnknown;
    std::uint64_t pos = te.body;
    R r;
    while (pos < te.end) {
      Elem e;
      if ((r = Header(pos, te, &e)) != R::kOk) return r;
      if (e.unknown) return Fail(R::kInvalid, "unknown-size TrackEntry child");
      switch (e.id) {
        case kIdTrackNumber:
          r = Uint(e, &number);
          break;
        case kIdTrackType:
          r = Uint(e, &type);
          break;
        case kIdDefaultDuration:
          r = Uint(e, &default_duration);
          break;
        case kIdCodecId:
          if ((r = Leaf(e)) == R::kOk) {
            codec = StrEq(e, "V_VP9")   ? Codec::kVp9
                    : StrEq(e, "V_VP8") ? Codec::kVp8
                                        : Codec::kUnknown;
          }
          break;
        case kIdContentEncodings:
          encoded = true;  // Compression or encryption of frame data.
          break;
        case kIdVideo: {
          has_video_elem = true;
          std::uint64_t vpos = e.body;
          while (r == R::kOk && vpos < e.end) {
            Elem v;
            if ((r = Header(vpos, e, &v)) != R::kOk) break;
            if (v.unknown) return Fail(R::kInvalid, "unknown-size Video child");
            if (v.id == kIdPixelWidth) r = Uint(v, &width);
            if (v.id == kIdPixelHeight) r = Uint(v, &height);
            if (v.id == kIdAlphaMode) r = Uint(v, &alpha_mode);
            vpos = v.end;
          }
          break;
        }
        default:
          r = R::kOk;
          break;
      }
      if (r != R::kOk) return r;
      pos = e.end;
    }

    if (type == kTrackTypeAudio) out_->has_audio = true;
    if (type != kTrackTypeVideo || out_->has_video) return R::kOk;
    if (codec == Codec::kUnknown) return R::kOk;
    if (number == 0 || !has_video_elem) {
      return Fail(R::kInvalid, "video track without number or Video element");
    }
    if (encoded)
      return Fail(R::kUnsupported, "ContentEncodings on video track");
    if (width == 0 || height == 0) {
      return Fail(R::kInvalid, "zero PixelWidth/PixelHeight");
    }
    if (width > limits_.max_dimension || height > limits_.max_dimension) {
      return Fail(R::kLimit, "dimensions over max_dimension");
    }
    out_->has_video = true;
    out_->video.number = number;
    out_->video.codec = codec;
    out_->video.width = static_cast<std::uint32_t>(width);
    out_->video.height = static_cast<std::uint32_t>(height);
    out_->video.alpha_mode = alpha_mode;
    out_->video.default_duration_ns = default_duration;
    return R::kOk;
  }

  static bool IsClusterChild(std::uint32_t id) {
    switch (id) {
      case kIdTimecode:
      case kIdPosition:
      case kIdPrevSize:
      case kIdSilentTracks:
      case kIdSimpleBlock:
      case kIdBlockGroup:
      case kIdEncryptedBlock:
      case kIdVoid:
      case kIdCrc32:
        return true;
      default:
        return false;
    }
  }

  // Parses a Cluster and sets *next to where the Segment continues.
  R ParseCluster(const Elem& cl, std::uint64_t* next) {
    bool have_tc = false;
    std::uint64_t tc = 0;
    std::uint64_t pos = cl.body;
    R r;
    while (pos < cl.end) {
      Elem e;
      if ((r = Header(pos, cl, &e)) != R::kOk) return r;
      if (cl.unknown && !IsClusterChild(e.id)) {
        *next = pos;  // An unknown-size Cluster ends at a non-child.
        return R::kOk;
      }
      if (e.unknown) return Fail(R::kInvalid, "unknown-size Cluster child");
      switch (e.id) {
        case kIdTimecode:
          r = Uint(e, &tc);
          have_tc = true;
          break;
        case kIdSimpleBlock:
        case kIdBlockGroup:
          if (!have_tc)
            return Fail(R::kInvalid, "block before Cluster Timecode");
          r = e.id == kIdSimpleBlock ? ParseSimpleBlock(e, tc)
                                     : ParseBlockGroup(e, tc);
          break;
        default:
          r = R::kOk;
          break;
      }
      if (r != R::kOk) return r;
      pos = e.end;
    }
    *next = cl.end;
    // Running past EOF inside a known-size Cluster is truncation.
    if (cl.truncated) return Fail(R::kTruncated, "Cluster runs past EOF");
    return R::kOk;
  }

  // Parses a Block/SimpleBlock header. Returns kOk with *mine == false for
  // other tracks.
  R BlockHeader(const Elem& e, std::uint64_t cluster_tc, bool* mine,
                FrameEntry* f, std::uint8_t* flags) {
    std::uint64_t pos = e.body;
    if (pos >= e.end) return Fail(R::kInvalid, "empty block");
    const std::uint8_t t0 = d_[pos];
    int len = 1;
    while (len <= 8 && !(t0 & (0x80 >> (len - 1)))) ++len;
    if (len > 8) return Fail(R::kInvalid, "bad track number");
    if (pos + len + 3 > e.end) return Fail(R::kInvalid, "short block header");
    std::uint64_t track = t0 & (0xFF >> len);
    for (int i = 1; i < len; ++i) track = (track << 8) | d_[pos + i];
    pos += len;
    const std::int16_t rel = static_cast<std::int16_t>(
        static_cast<std::uint16_t>((d_[pos] << 8) | d_[pos + 1]));
    *flags = d_[pos + 2];
    pos += 3;
    *mine = out_->has_video && track == out_->video.number;
    if (!*mine) return R::kOk;
    if (*flags & 0x06) return Fail(R::kUnsupported, "laced video block");
    if (pos >= e.end) return Fail(R::kInvalid, "empty frame");
    f->data = Span{pos, e.end - pos};
    // cluster_tc is < 2^64 and rel is 16-bit; keep the maths in int64 and
    // reject values that can't be represented.
    if (cluster_tc > (1ull << 62)) return Fail(R::kInvalid, "huge timecode");
    const std::int64_t tc = static_cast<std::int64_t>(cluster_tc) + rel;
    const std::uint64_t scale = out_->timecode_scale_ns;
    if (tc != 0 && scale > static_cast<std::uint64_t>(INT64_MAX) /
                               static_cast<std::uint64_t>(tc < 0 ? -tc : tc)) {
      return Fail(R::kInvalid, "timestamp overflow");
    }
    f->pts_ns = tc * static_cast<std::int64_t>(scale);
    return R::kOk;
  }

  R Push(const FrameEntry& f) {
    if (out_->frames.size() >= limits_.max_frames) {
      return Fail(R::kLimit, "too many frames");
    }
    out_->frames.push_back(f);
    return R::kOk;
  }

  R ParseSimpleBlock(const Elem& e, std::uint64_t tc) {
    bool mine = false;
    std::uint8_t flags = 0;
    FrameEntry f;
    R r = BlockHeader(e, tc, &mine, &f, &flags);
    if (r != R::kOk || !mine) return r;
    f.keyframe = (flags & 0x80) != 0;
    return Push(f);
  }

  R ParseBlockGroup(const Elem& g, std::uint64_t tc) {
    // Header() already refused a truncated group, which could be missing its
    // alpha or its ReferenceBlock.
    R r = R::kOk;
    bool have_block = false, mine = false, referenced = false;
    FrameEntry f;
    std::uint64_t pos = g.body;
    while (pos < g.end) {
      Elem e;
      if ((r = Header(pos, g, &e)) != R::kOk) return r;
      if (e.unknown) return Fail(R::kInvalid, "unknown-size BlockGroup child");
      switch (e.id) {
        case kIdBlock: {
          if (have_block)
            return Fail(R::kInvalid, "two Blocks in a BlockGroup");
          have_block = true;
          std::uint8_t flags = 0;
          r = BlockHeader(e, tc, &mine, &f, &flags);
          break;
        }
        case kIdReferenceBlock:
          referenced = true;
          break;
        case kIdBlockAdditions:
          r = ParseBlockAdditions(e, &f.alpha);
          break;
        default:
          break;
      }
      if (r != R::kOk) return r;
      pos = e.end;
    }
    if (!have_block || !mine) return R::kOk;
    f.keyframe = !referenced;
    return Push(f);
  }

  R ParseBlockAdditions(const Elem& adds, Span* alpha) {
    std::uint64_t pos = adds.body;
    std::size_t mores = 0;
    R r;
    while (pos < adds.end) {
      Elem more;
      if ((r = Header(pos, adds, &more)) != R::kOk) return r;
      if (more.unknown) return Fail(R::kInvalid, "unknown-size BlockMore");
      if (more.id == kIdBlockMore) {
        if (++mores > kMaxBlockMores)
          return Fail(R::kLimit, "too many BlockMore");
        std::uint64_t add_id = 1;  // BlockAddID defaults to 1.
        Span data;
        std::uint64_t mpos = more.body;
        while (mpos < more.end) {
          Elem c;
          if ((r = Header(mpos, more, &c)) != R::kOk) return r;
          if (c.unknown)
            return Fail(R::kInvalid, "unknown-size BlockMore child");
          if (c.id == kIdBlockAddId && (r = Uint(c, &add_id)) != R::kOk)
            return r;
          if (c.id == kIdBlockAdditional) data = Span{c.body, c.end - c.body};
          mpos = c.end;
        }
        if (add_id == 1 && data.present() && !alpha->present()) *alpha = data;
      }
      pos = more.end;
    }
    return R::kOk;
  }

  const std::uint8_t* d_;
  std::size_t n_;
  Limits limits_;
  FrameIndex* out_;
  const char* why_ = "";
};

}  // namespace

FrameIndex IndexWithMiniEbml(const std::uint8_t* data, std::size_t size,
                             const Limits& limits) {
  FrameIndex out;
  Parser p(data, size, limits, &out);
  const R r = p.Run();
  switch (r) {
    case R::kOk:
      out.status = IndexStatus::kOk;
      break;
    case R::kTruncated:
      out.status = IndexStatus::kTruncated;
      break;
    case R::kInvalid:
      out.status = IndexStatus::kInvalid;
      break;
    case R::kUnsupported:
      out.status = IndexStatus::kUnsupported;
      break;
    case R::kLimit:
      out.status = IndexStatus::kLimit;
      break;
  }
  if (r != R::kOk) out.error = p.why();
  if (out.status == IndexStatus::kOk && !out.has_video) {
    out.status = IndexStatus::kUnsupported;
    out.error = "no VP8/VP9 video track";
  }
  return out;
}

}  // namespace spike
