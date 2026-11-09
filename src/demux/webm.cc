// WebM demuxer (see webm.h). Ported from spikes/alpha/demux_mini_ebml.cc and
// hardened with the checks listed under "Pitfalls" in docs/spikes/A-alpha.md.
#include "demux/webm.h"

#include <climits>
#include <cstddef>
#include <cstring>

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"

namespace wt {
namespace {

enum : uint32_t {
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
  kIdBlockDuration = 0x9B,
  kIdBlockAdditions = 0x75A1,
  kIdBlockMore = 0xA6,
  kIdBlockAddId = 0xEE,
  kIdBlockAdditional = 0xA5,
  kIdReferenceBlock = 0xFB,
  kIdVoid = 0xEC,
  kIdCrc32 = 0xBF,
};

constexpr uint64_t kTrackTypeVideo = 1;
constexpr uint64_t kTrackTypeAudio = 2;
// More BlockMores than this in one BlockGroup is not a sticker.
constexpr size_t kMaxBlockMores = 8;
// Used when neither the next timestamp, BlockDuration nor DefaultDuration
// gives a frame's duration.
constexpr int64_t kFallbackDurationNs = 1000000000 / 30;
// Cluster Timecode values above this are rejected, so adding a 16-bit block
// offset can't overflow int64.
constexpr uint64_t kMaxClusterTimecode = uint64_t{1} << 62;

struct Elem {
  uint32_t id = 0;
  uint64_t body = 0;       // First body byte.
  uint64_t end = 0;        // One past the body, clamped to the parent.
  bool unknown = false;    // Size was "unknown" (all value bits set).
  bool open_end = false;   // `end` is EOF, not a declared size.
  bool truncated = false;  // A Segment/Cluster whose size runs past EOF.
};

// value x scale as int64, or false if it doesn't fit.
bool ScaleTicks(int64_t value, uint64_t scale, int64_t* out) {
  const uint64_t mag = value < 0 ? uint64_t{0} - static_cast<uint64_t>(value)
                                 : static_cast<uint64_t>(value);
  if (mag != 0 && scale > static_cast<uint64_t>(INT64_MAX) / mag) return false;
  const int64_t product = static_cast<int64_t>(mag * scale);
  *out = value < 0 ? -product : product;
  return true;
}

bool PeekKeyframe(wt_codec codec, ByteView bytes, Span span) {
  if (span.empty() || span.size > UINT_MAX) return false;
  vpx_codec_iface_t* iface =
      codec == WT_CODEC_VP9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();
  vpx_codec_stream_info_t si;
  std::memset(&si, 0, sizeof(si));
  si.sz = sizeof(si);
  // DemuxWebm only records spans inside `bytes`.
  const uint8_t* data = bytes.data + span.offset;
  if (vpx_codec_peek_stream_info(iface, data,
                                 static_cast<unsigned int>(span.size),
                                 &si) != VPX_CODEC_OK) {
    return false;
  }
  return si.is_kf != 0;
}

// Reads a variable-length integer's length from its first byte: 1 + the
// number of leading zero bits, or max_len + 1 when it is too long.
int VintLength(uint8_t first, int max_len) {
  int len = 1;
  while (len <= max_len && (first & (0x80 >> (len - 1))) == 0) ++len;
  return len;
}

class Parser {
 public:
  Parser(ByteView bytes, const wt_limits& limits, WebmIndex* out)
      : d_(bytes.data), n_(bytes.size), limits_(limits), out_(out) {}

  wt_status Run() {
    Elem file;
    file.end = n_;
    file.open_end = true;

    // Check the magic first, so a short non-WebM input is invalid rather
    // than "truncated".
    static const uint8_t kMagic[4] = {0x1A, 0x45, 0xDF, 0xA3};
    const size_t magic_len = n_ < sizeof(kMagic) ? n_ : sizeof(kMagic);
    if (magic_len > 0 && std::memcmp(d_, kMagic, magic_len) != 0) {
      return Fail(WT_ERR_INVALID_DATA, "no EBML header");
    }
    Elem ebml;
    wt_status s = Header(0, file, &ebml);
    if (s != WT_OK) return s;
    if (ebml.id != kIdEbml) return Fail(WT_ERR_INVALID_DATA, "no EBML header");
    if ((s = ParseEbmlHeader(ebml)) != WT_OK) return s;

    // Only the first Segment is read; anything after it is ignored.
    uint64_t pos = ebml.end;
    while (pos < n_) {
      Elem e;
      if ((s = Header(pos, file, &e)) != WT_OK) return s;
      if (e.id == kIdSegment) return ParseSegment(e);
      if (e.unknown) {
        return Fail(WT_ERR_INVALID_DATA, "unknown-size top-level element");
      }
      pos = e.end;
    }
    // The loop only ends at EOF, so the file was cut before its Segment.
    return Fail(WT_ERR_TRUNCATED, "EOF before the Segment");
  }

  const char* why() const { return why_; }

 private:
  wt_status Fail(wt_status s, const char* why) {
    why_ = why;
    return s;
  }

  // Reads the element header at `pos` inside `parent`. On WT_OK, *e fits
  // inside the parent. Unknown sizes are accepted only for Segment and
  // Cluster; such an element ends with its parent.
  wt_status Header(uint64_t pos, const Elem& parent, Elem* e) {
    const wt_status overflow =
        parent.open_end ? WT_ERR_TRUNCATED : WT_ERR_INVALID_DATA;
    if (pos >= parent.end) return Fail(overflow, "element header past parent");

    // ID: 1-4 bytes, length from the leading zeros, marker bit kept.
    const int id_len = VintLength(d_[pos], 4);
    if (id_len > 4) return Fail(WT_ERR_INVALID_DATA, "bad element ID");
    if (static_cast<uint64_t>(id_len) > parent.end - pos) {
      return Fail(overflow, "element ID past parent");
    }
    uint32_t id = 0;
    for (int i = 0; i < id_len; ++i) id = (id << 8) | d_[pos++];

    // Size: 1-8 bytes. All value bits set means "unknown".
    if (pos >= parent.end) return Fail(overflow, "element size past parent");
    const int size_len = VintLength(d_[pos], 8);
    if (size_len > 8) return Fail(WT_ERR_INVALID_DATA, "bad element size");
    if (static_cast<uint64_t>(size_len) > parent.end - pos) {
      return Fail(overflow, "element size past parent");
    }
    const uint8_t value_mask = static_cast<uint8_t>(0xFF >> size_len);
    uint64_t size = d_[pos] & value_mask;
    bool all_ones = size == value_mask;
    for (int i = 1; i < size_len; ++i) {
      size = (size << 8) | d_[pos + static_cast<uint64_t>(i)];
      all_ones = all_ones && d_[pos + static_cast<uint64_t>(i)] == 0xFF;
    }
    pos += static_cast<uint64_t>(size_len);

    e->id = id;
    e->body = pos;
    e->unknown = all_ones;
    e->truncated = false;
    if (all_ones) {
      if (id != kIdSegment && id != kIdCluster) {
        return Fail(WT_ERR_INVALID_DATA, "unknown size on a non-master");
      }
      e->end = parent.end;
      e->open_end = parent.open_end;
      return WT_OK;
    }
    // pos <= parent.end <= n_, so this compare can't overflow.
    if (size > parent.end - pos) {
      if (!parent.open_end) {
        return Fail(WT_ERR_INVALID_DATA, "element overflows its parent");
      }
      // Runs past EOF. Segment and Cluster are read up to EOF so the frames
      // before the cut survive; any other element is unusable.
      if (id != kIdSegment && id != kIdCluster) {
        return Fail(WT_ERR_TRUNCATED, "EOF inside an element");
      }
      e->end = parent.end;
      e->open_end = true;
      e->truncated = true;
      return WT_OK;
    }
    e->end = pos + size;
    e->open_end = false;
    return WT_OK;
  }

  wt_status Uint(const Elem& e, uint64_t* v) {
    if (e.unknown) return Fail(WT_ERR_INVALID_DATA, "unknown-size integer");
    const uint64_t len = e.end - e.body;
    if (len > 8) return Fail(WT_ERR_INVALID_DATA, "integer over 8 bytes");
    uint64_t x = 0;
    for (uint64_t i = 0; i < len; ++i) x = (x << 8) | d_[e.body + i];
    *v = x;
    return WT_OK;
  }

  // Matroska strings may be NUL padded.
  bool StrEq(const Elem& e, const char* s) const {
    const size_t len = std::strlen(s);
    uint64_t n = e.end - e.body;
    while (n > 0 && d_[e.body + n - 1] == 0) --n;
    return n == len && std::memcmp(d_ + e.body, s, len) == 0;
  }

  wt_status ParseEbmlHeader(const Elem& ebml) {
    if (ebml.unknown) return Fail(WT_ERR_INVALID_DATA, "unknown-size EBML");
    bool doctype_ok = false;
    uint64_t pos = ebml.body;
    while (pos < ebml.end) {
      Elem e;
      const wt_status s = Header(pos, ebml, &e);
      if (s != WT_OK) return s;
      if (e.unknown)
        return Fail(WT_ERR_INVALID_DATA, "unknown-size EBML child");
      if (e.id == kIdDocType) {
        doctype_ok = StrEq(e, "webm") || StrEq(e, "matroska");
      }
      pos = e.end;
    }
    return doctype_ok ? WT_OK
                      : Fail(WT_ERR_INVALID_DATA, "DocType is not webm");
  }

  wt_status ParseSegment(const Elem& seg) {
    bool seen_cluster = false;
    uint64_t pos = seg.body;
    while (pos < seg.end) {
      Elem e;
      wt_status s = Header(pos, seg, &e);
      if (s != WT_OK) return s;
      if (e.unknown && e.id != kIdCluster) {
        return Fail(WT_ERR_INVALID_DATA, "unknown-size Segment child");
      }
      switch (e.id) {
        // Info or Tracks after the first Cluster would retime or reassign
        // blocks already indexed; they are skipped.
        case kIdInfo:
          if (!seen_cluster) s = ParseInfo(e);
          break;
        case kIdTracks:
          if (!seen_cluster) s = ParseTracks(e);
          break;
        case kIdCluster:
          seen_cluster = true;
          if ((s = ParseCluster(e, &pos)) != WT_OK) return s;
          continue;  // ParseCluster set pos.
        default:
          break;  // SeekHead, Cues, Tags, Void, ... skipped by size.
      }
      if (s != WT_OK) return s;
      pos = e.end;
    }
    // A Segment whose size runs past EOF is a truncated file, even if the
    // last element happened to end exactly at EOF.
    if (seg.truncated) return Fail(WT_ERR_TRUNCATED, "Segment runs past EOF");
    return WT_OK;
  }

  wt_status ParseInfo(const Elem& info) {
    uint64_t pos = info.body;
    while (pos < info.end) {
      Elem e;
      wt_status s = Header(pos, info, &e);
      if (s != WT_OK) return s;
      if (e.id == kIdTimecodeScale) {
        uint64_t v = 0;
        if ((s = Uint(e, &v)) != WT_OK) return s;
        if (v == 0) return Fail(WT_ERR_INVALID_DATA, "TimecodeScale is 0");
        out_->timecode_scale_ns = v;
      }
      pos = e.end;
    }
    return WT_OK;
  }

  wt_status ParseTracks(const Elem& tracks) {
    uint64_t pos = tracks.body;
    while (pos < tracks.end) {
      Elem e;
      wt_status s = Header(pos, tracks, &e);
      if (s != WT_OK) return s;
      if (e.id == kIdTrackEntry && (s = ParseTrackEntry(e)) != WT_OK) return s;
      pos = e.end;
    }
    return WT_OK;
  }

  wt_status ParseVideo(const Elem& video, uint64_t* width, uint64_t* height,
                       uint64_t* alpha_mode) {
    uint64_t pos = video.body;
    while (pos < video.end) {
      Elem v;
      wt_status s = Header(pos, video, &v);
      if (s != WT_OK) return s;
      if (v.id == kIdPixelWidth) s = Uint(v, width);
      if (v.id == kIdPixelHeight) s = Uint(v, height);
      if (v.id == kIdAlphaMode) s = Uint(v, alpha_mode);
      if (s != WT_OK) return s;
      pos = v.end;
    }
    return WT_OK;
  }

  wt_status ParseTrackEntry(const Elem& te) {
    uint64_t number = 0, type = 0, default_duration = 0;
    uint64_t width = 0, height = 0, alpha_mode = 0;
    bool has_video_elem = false, encoded = false;
    wt_codec codec = WT_CODEC_UNKNOWN;
    uint64_t pos = te.body;
    while (pos < te.end) {
      Elem e;
      wt_status s = Header(pos, te, &e);
      if (s != WT_OK) return s;
      switch (e.id) {
        case kIdTrackNumber:
          s = Uint(e, &number);
          break;
        case kIdTrackType:
          s = Uint(e, &type);
          break;
        case kIdDefaultDuration:
          s = Uint(e, &default_duration);
          break;
        case kIdCodecId:
          codec = StrEq(e, "V_VP9")   ? WT_CODEC_VP9
                  : StrEq(e, "V_VP8") ? WT_CODEC_VP8
                                      : WT_CODEC_UNKNOWN;
          break;
        case kIdContentEncodings:
          encoded = true;  // Compression or encryption of frame data.
          break;
        case kIdVideo:
          has_video_elem = true;
          s = ParseVideo(e, &width, &height, &alpha_mode);
          break;
        default:
          break;
      }
      if (s != WT_OK) return s;
      pos = e.end;
    }

    if (type == kTrackTypeAudio) out_->has_audio = true;
    // The first VP8/VP9 video track wins; others are ignored like audio.
    if (type != kTrackTypeVideo || codec == WT_CODEC_UNKNOWN ||
        out_->has_video) {
      return WT_OK;
    }
    if (number == 0 || !has_video_elem) {
      return Fail(WT_ERR_INVALID_DATA, "video track without number or Video");
    }
    if (encoded) {
      return Fail(WT_ERR_UNSUPPORTED, "ContentEncodings on the video track");
    }
    if (width == 0 || height == 0) {
      return Fail(WT_ERR_INVALID_DATA, "zero PixelWidth or PixelHeight");
    }
    if (width > limits_.max_dimension || height > limits_.max_dimension) {
      return Fail(WT_ERR_LIMIT, "video size over max_dimension");
    }
    out_->has_video = true;
    WebmVideoTrack& v = out_->video;
    v.number = number;
    v.codec = codec;
    v.width = static_cast<uint32_t>(width);
    v.height = static_cast<uint32_t>(height);
    v.alpha_mode = alpha_mode;
    v.default_duration_ns = default_duration <= static_cast<uint64_t>(INT64_MAX)
                                ? static_cast<int64_t>(default_duration)
                                : 0;
    return WT_OK;
  }

  static bool IsClusterChild(uint32_t id) {
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
  wt_status ParseCluster(const Elem& cl, uint64_t* next) {
    bool have_tc = false;
    uint64_t tc = 0;
    uint64_t pos = cl.body;
    while (pos < cl.end) {
      Elem e;
      wt_status s = Header(pos, cl, &e);
      if (s != WT_OK) return s;
      if (cl.unknown && !IsClusterChild(e.id)) {
        *next = pos;  // An unknown-size Cluster ends at a non-child.
        return WT_OK;
      }
      if (e.unknown) {
        return Fail(WT_ERR_INVALID_DATA, "unknown-size Cluster child");
      }
      switch (e.id) {
        case kIdTimecode:
          s = Uint(e, &tc);
          if (s == WT_OK && tc > kMaxClusterTimecode) {
            s = Fail(WT_ERR_INVALID_DATA, "Cluster Timecode too large");
          }
          have_tc = true;
          break;
        case kIdSimpleBlock:
        case kIdBlockGroup:
          if (!have_tc) {
            return Fail(WT_ERR_INVALID_DATA, "block before Cluster Timecode");
          }
          s = e.id == kIdSimpleBlock ? ParseSimpleBlock(e, tc)
                                     : ParseBlockGroup(e, tc);
          break;
        default:
          break;
      }
      if (s != WT_OK) return s;
      pos = e.end;
    }
    *next = cl.end;
    // Running past EOF inside a known-size Cluster is truncation.
    if (cl.truncated) return Fail(WT_ERR_TRUNCATED, "Cluster runs past EOF");
    return WT_OK;
  }

  // Parses a Block/SimpleBlock header. Sets *mine to false, and touches
  // nothing else, for blocks of other tracks.
  wt_status BlockHeader(const Elem& e, uint64_t cluster_tc, bool* mine,
                        WebmFrame* f, uint8_t* flags) {
    uint64_t pos = e.body;
    if (pos >= e.end) return Fail(WT_ERR_INVALID_DATA, "empty block");
    const int len = VintLength(d_[pos], 8);
    if (len > 8) return Fail(WT_ERR_INVALID_DATA, "bad block track number");
    if (static_cast<uint64_t>(len) + 3 > e.end - pos) {
      return Fail(WT_ERR_INVALID_DATA, "short block header");
    }
    uint64_t track = d_[pos] & (0xFFu >> len);
    for (int i = 1; i < len; ++i) {
      track = (track << 8) | d_[pos + static_cast<uint64_t>(i)];
    }
    pos += static_cast<uint64_t>(len);
    const int16_t rel = static_cast<int16_t>(
        static_cast<uint16_t>((d_[pos] << 8) | d_[pos + 1]));
    const uint8_t block_flags = d_[pos + 2];
    pos += 3;

    *mine = out_->has_video && track == out_->video.number;
    if (!*mine) return WT_OK;
    // BlockAdditions apply to a whole Block, so alpha on a laced block would
    // be ambiguous (pitfall 4). Muxers never lace video.
    if ((block_flags & 0x06) != 0) {
      return Fail(WT_ERR_UNSUPPORTED, "laced video block");
    }
    if (pos >= e.end) return Fail(WT_ERR_INVALID_DATA, "empty video frame");
    f->data = Span{pos, e.end - pos};
    // cluster_tc <= 2^62 (checked when read), so this can't overflow.
    const int64_t tc = static_cast<int64_t>(cluster_tc) + rel;
    if (!ScaleTicks(tc, out_->timecode_scale_ns, &f->pts_ns)) {
      return Fail(WT_ERR_INVALID_DATA, "timestamp overflows int64 ns");
    }
    *flags = block_flags;
    return WT_OK;
  }

  wt_status Push(WebmFrame f, int64_t block_duration_ns) {
    if (out_->frames.size() >= limits_.max_frames) {
      return Fail(WT_ERR_LIMIT, "more frames than max_frames");
    }
    // Pitfall 3: BlockAdditions mean alpha only when AlphaMode is 1.
    if (!out_->video.has_alpha()) f.alpha = Span{};
    // Pitfall 1: the streams come from independent encoders, so a frame is a
    // random-access point only if both payloads are bitstream keyframes.
    const ByteView bytes{d_, n_};
    f.random_access =
        PeekKeyframe(out_->video.codec, bytes, f.data) &&
        (f.alpha.empty() || PeekKeyframe(out_->video.codec, bytes, f.alpha));
    // Held here until the next frame's pts is known; see Finish().
    f.duration_ns = block_duration_ns;
    out_->frames.push_back(f);
    return WT_OK;
  }

  wt_status ParseSimpleBlock(const Elem& e, uint64_t tc) {
    bool mine = false;
    uint8_t flags = 0;
    WebmFrame f;
    const wt_status s = BlockHeader(e, tc, &mine, &f, &flags);
    if (s != WT_OK || !mine) return s;
    f.keyframe = (flags & 0x80) != 0;
    return Push(f, 0);
  }

  wt_status ParseBlockGroup(const Elem& g, uint64_t tc) {
    // Header() already refused a group that runs past EOF, which could be
    // missing its alpha or its ReferenceBlock (pitfall 13).
    bool have_block = false, mine = false, referenced = false;
    uint64_t block_duration = 0;
    WebmFrame f;
    uint64_t pos = g.body;
    while (pos < g.end) {
      Elem e;
      wt_status s = Header(pos, g, &e);
      if (s != WT_OK) return s;
      switch (e.id) {
        case kIdBlock: {
          if (have_block) {
            return Fail(WT_ERR_INVALID_DATA, "two Blocks in a BlockGroup");
          }
          have_block = true;
          uint8_t flags = 0;
          s = BlockHeader(e, tc, &mine, &f, &flags);
          break;
        }
        case kIdBlockDuration:
          s = Uint(e, &block_duration);
          break;
        case kIdReferenceBlock:
          referenced = true;
          break;
        case kIdBlockAdditions:
          s = ParseBlockAdditions(e, &f.alpha);
          break;
        default:
          break;
      }
      if (s != WT_OK) return s;
      pos = e.end;
    }
    if (!have_block || !mine) return WT_OK;
    f.keyframe = !referenced;
    // A BlockDuration that doesn't fit in int64 ns is ignored like a missing
    // one; it only matters for the last frame.
    int64_t duration_ns = 0;
    if (block_duration > static_cast<uint64_t>(INT64_MAX) ||
        !ScaleTicks(static_cast<int64_t>(block_duration),
                    out_->timecode_scale_ns, &duration_ns)) {
      duration_ns = 0;
    }
    return Push(f, duration_ns);
  }

  wt_status ParseBlockAdditions(const Elem& adds, Span* alpha) {
    size_t mores = 0;
    uint64_t pos = adds.body;
    while (pos < adds.end) {
      Elem more;
      wt_status s = Header(pos, adds, &more);
      if (s != WT_OK) return s;
      if (more.id == kIdBlockMore) {
        if (++mores > kMaxBlockMores) {
          return Fail(WT_ERR_LIMIT, "too many BlockMores");
        }
        uint64_t add_id = 1;  // BlockAddID defaults to 1.
        Span data;
        uint64_t mpos = more.body;
        while (mpos < more.end) {
          Elem c;
          if ((s = Header(mpos, more, &c)) != WT_OK) return s;
          if (c.id == kIdBlockAddId && (s = Uint(c, &add_id)) != WT_OK) {
            return s;
          }
          if (c.id == kIdBlockAdditional) data = Span{c.body, c.end - c.body};
          mpos = c.end;
        }
        if (add_id == 1 && !data.empty() && alpha->empty()) *alpha = data;
      }
      pos = more.end;
    }
    return WT_OK;
  }

  const uint8_t* d_;
  size_t n_;
  const wt_limits& limits_;
  WebmIndex* out_;
  const char* why_ = "";
};

// Turns the BlockDuration stashed in duration_ns into each frame's display
// duration (see WebmFrame::duration_ns).
void FinishDurations(WebmIndex* index) {
  std::vector<WebmFrame>& frames = index->frames;
  const int64_t nominal = index->video.default_duration_ns > 0
                              ? index->video.default_duration_ns
                              : kFallbackDurationNs;
  for (size_t i = 0; i < frames.size(); ++i) {
    const int64_t block_duration = frames[i].duration_ns;
    int64_t d = 0;
    if (i + 1 < frames.size()) {
      const int64_t a = frames[i].pts_ns;
      const int64_t b = frames[i + 1].pts_ns;
      // Overflow-safe b - a, kept only when positive.
      if (b > a && (a >= 0 || b <= INT64_MAX + a)) d = b - a;
    }
    if (d <= 0) d = block_duration;
    if (d <= 0) d = nominal;
    frames[i].duration_ns = d;
  }
}

}  // namespace

wt_status DemuxWebm(ByteView bytes, const wt_limits& limits, WebmIndex* out) {
  *out = WebmIndex();
  if ((bytes.data == nullptr && bytes.size != 0) || limits.max_frames == 0 ||
      limits.max_dimension == 0) {
    out->status = WT_ERR_INVALID_ARGUMENT;
    out->error = "bad arguments";
    return out->status;
  }
  Parser parser(bytes, limits, out);
  wt_status s = parser.Run();
  out->error = s == WT_OK ? "" : parser.why();
  if (s == WT_OK && !out->has_video) {
    s = WT_ERR_UNSUPPORTED;
    out->error = "no VP8/VP9 video track";
  } else if (s == WT_OK && out->frames.empty()) {
    s = WT_ERR_INVALID_DATA;
    out->error = "video track has no frames";
  }
  if (s != WT_OK && s != WT_ERR_TRUNCATED) out->frames.clear();
  FinishDurations(out);
  out->status = s;
  return s;
}

}  // namespace wt
