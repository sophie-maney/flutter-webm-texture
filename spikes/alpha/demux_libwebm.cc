// Spike A (T0.7): frame indexing on top of libwebm's webm_parser.
//
// webm_parser is a push/callback parser. We feed it a reader over the
// in-memory file and record byte spans:
//   - colour payload: OnFrame() gives the absolute position and size,
//   - alpha payload: OnElementBegin(BlockAdditional) gives the element's
//     position; we skip its body so webm_parser never copies it.
// BlockAddID is a sibling of BlockAdditional that may come before or after
// it, so the IDs are matched by BlockMore order in OnBlockGroupEnd().
//
// Hardening that webm_parser needs from its caller (found in this spike):
//   - It sizes byte/string elements with vector::resize(declared size) before
//     reading them. Inside an unknown-size Segment/Cluster nothing bounds that
//     size, so a 40-byte file can request gigabytes. We cap leaf element
//     sizes in OnElementBegin(), which runs before the parser allocates.
//   - A Cluster that is read (not skipped) stores every SimpleBlock and
//     BlockGroup it contains in its DOM until the cluster ends. An empty
//     BlockGroup is 2 bytes on disk and a few hundred in memory, so we cap
//     the number of elements as well.
#include <cstring>
#include <limits>

#include "frame_index.h"
#include "webm/callback.h"
#include "webm/reader.h"
#include "webm/status.h"
#include "webm/webm_parser.h"

namespace spike {
namespace {

// User status codes must be positive (webm/status.h).
constexpr std::int32_t kErrUnsupported = 1;
constexpr std::int32_t kErrLimit = 2;
constexpr std::int32_t kErrInvalid = 3;

// Upper bound on elements we let webm_parser materialise: a BlockGroup with
// alpha is about 7 elements, so 16 per allowed frame plus headers is
// generous for real files and keeps the Cluster DOM to a few MB.
std::uint64_t MaxElements(const Limits& l) { return 16 * l.max_frames + 1024; }

class SpanReader : public webm::Reader {
 public:
  SpanReader(const std::uint8_t* data, std::size_t size)
      : data_(data), size_(size) {}

  webm::Status Read(std::size_t num_to_read, std::uint8_t* buffer,
                    std::uint64_t* num_actually_read) override {
    const std::size_t n = Clamp(num_to_read);
    if (n > 0) std::memcpy(buffer, data_ + pos_, n);
    return Advance(n, num_to_read, num_actually_read);
  }

  webm::Status Skip(std::uint64_t num_to_skip,
                    std::uint64_t* num_actually_skipped) override {
    return Advance(Clamp(num_to_skip), num_to_skip, num_actually_skipped);
  }

  std::uint64_t Position() const override { return pos_; }

 private:
  std::size_t Clamp(std::uint64_t want) const {
    const std::uint64_t left = size_ - pos_;
    return static_cast<std::size_t>(want < left ? want : left);
  }
  webm::Status Advance(std::size_t n, std::uint64_t want, std::uint64_t* got) {
    pos_ += n;
    *got = n;
    if (n == want) return webm::Status(webm::Status::kOkCompleted);
    if (n > 0) return webm::Status(webm::Status::kOkPartial);
    return webm::Status(webm::Status::kEndOfFile);
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

bool IsMaster(webm::Id id) {
  switch (id) {
    case webm::Id::kSegment:
    case webm::Id::kCluster:
    case webm::Id::kBlockGroup:
    case webm::Id::kBlockAdditions:
    case webm::Id::kBlockMore:
    case webm::Id::kTracks:
    case webm::Id::kTrackEntry:
    case webm::Id::kVideo:
    case webm::Id::kAudio:
    case webm::Id::kInfo:
    case webm::Id::kEbml:
      return true;
    default:
      return false;
  }
}

class IndexCallback : public webm::Callback {
 public:
  IndexCallback(std::size_t file_size, const Limits& limits, FrameIndex* out)
      : file_size_(file_size), limits_(limits), out_(out) {}

  webm::Status OnElementBegin(const webm::ElementMetadata& meta,
                              webm::Action* action) override {
    *action = webm::Action::kRead;
    // webm_parser accepts any top-level element, so random bytes parse as
    // some element whose size runs past EOF ("truncated"). Require the EBML
    // header first, as the mini reader does.
    if (meta.position == 0 && meta.id != webm::Id::kEbml) {
      return Fail(kErrInvalid, "no EBML header");
    }
    if (++elements_ > MaxElements(limits_) && !g_libwebm_unguarded) {
      return Fail(kErrLimit, "too many elements");
    }
    const bool unknown = meta.size == webm::kUnknownElementSize;
    if (!g_libwebm_unguarded && !unknown && !IsMaster(meta.id) &&
        meta.id != webm::Id::kSimpleBlock && meta.id != webm::Id::kBlock) {
      // Leaf elements are read into memory by webm_parser. Refuse any that
      // are bigger than the cap or than what is left of the file.
      if (meta.size > limits_.max_leaf_bytes) {
        return Fail(kErrLimit, "leaf element larger than max_leaf_bytes");
      }
      if (meta.position + meta.header_size + meta.size > file_size_) {
        return webm::Status(webm::Status::kEndOfFile);
      }
    }
    switch (meta.id) {
      // Never needed for playback; skipping keeps their DOMs out of memory.
      case webm::Id::kCues:
      case webm::Id::kTags:
      case webm::Id::kChapters:
      case webm::Id::kSeekHead:
        *action = webm::Action::kSkip;
        break;
      case webm::Id::kBlockMore:
        if (in_group_) {
          if (mores_.size() >= 8) return Fail(kErrLimit, "too many BlockMore");
          mores_.push_back(Span{});
        }
        break;
      case webm::Id::kBlockAdditional:
        if (in_group_ && !mores_.empty()) {
          mores_.back() = Span{meta.position + meta.header_size, meta.size};
        }
        // We only need the span. Skipping stops webm_parser copying it.
        *action = webm::Action::kSkip;
        break;
      default:
        break;
    }
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnEbml(const webm::ElementMetadata&,
                      const webm::Ebml& ebml) override {
    const std::string& t = ebml.doc_type.value();
    if (t != "webm" && t != "matroska") {
      return Fail(kErrInvalid, "DocType is not webm");
    }
    saw_ebml_ = true;
    return Ok();
  }

  webm::Status OnSegmentBegin(const webm::ElementMetadata&,
                              webm::Action* action) override {
    *action = webm::Action::kRead;
    if (!saw_ebml_) return Fail(kErrInvalid, "no EBML header");
    return Ok();
  }

  webm::Status OnInfo(const webm::ElementMetadata&,
                      const webm::Info& info) override {
    if (info.timecode_scale.value() == 0) {
      return Fail(kErrInvalid, "TimecodeScale is 0");
    }
    out_->timecode_scale_ns = info.timecode_scale.value();
    return Ok();
  }

  webm::Status OnTrackEntry(const webm::ElementMetadata&,
                            const webm::TrackEntry& t) override {
    if (t.track_type.value() == webm::TrackType::kAudio) {
      out_->has_audio = true;
      return Ok();
    }
    if (t.track_type.value() != webm::TrackType::kVideo || out_->has_video) {
      return Ok();
    }
    const std::string& id = t.codec_id.value();
    Codec codec = id == "V_VP9"   ? Codec::kVp9
                  : id == "V_VP8" ? Codec::kVp8
                                  : Codec::kUnknown;
    if (codec == Codec::kUnknown) return Ok();  // Look for another track.
    if (t.track_number.value() == 0 || !t.video.is_present()) {
      return Fail(kErrInvalid, "video track without number or Video element");
    }
    if (t.content_encodings.is_present()) {
      // Compressed (header stripping) or encrypted frames.
      return Fail(kErrUnsupported, "ContentEncodings on video track");
    }
    const webm::Video& v = t.video.value();
    if (v.pixel_width.value() == 0 || v.pixel_height.value() == 0) {
      return Fail(kErrInvalid, "zero PixelWidth/PixelHeight");
    }
    if (v.pixel_width.value() > limits_.max_dimension ||
        v.pixel_height.value() > limits_.max_dimension) {
      return Fail(kErrLimit, "dimensions over max_dimension");
    }
    out_->has_video = true;
    out_->video.number = t.track_number.value();
    out_->video.codec = codec;
    out_->video.width = static_cast<std::uint32_t>(v.pixel_width.value());
    out_->video.height = static_cast<std::uint32_t>(v.pixel_height.value());
    out_->video.alpha_mode = v.alpha_mode.value();
    out_->video.default_duration_ns = t.default_duration.value();
    return Ok();
  }

  webm::Status OnClusterBegin(const webm::ElementMetadata&,
                              const webm::Cluster& cluster,
                              webm::Action* action) override {
    *action = webm::Action::kRead;
    if (!cluster.timecode.is_present()) {
      return Fail(kErrInvalid, "Cluster without Timecode before its blocks");
    }
    cluster_timecode_ = cluster.timecode.value();
    return Ok();
  }

  webm::Status OnSimpleBlockBegin(const webm::ElementMetadata&,
                                  const webm::SimpleBlock& block,
                                  webm::Action* action) override {
    webm::Status s = BeginBlock(block, action);
    pending_.keyframe = block.is_key_frame;
    return s;
  }

  webm::Status OnSimpleBlockEnd(const webm::ElementMetadata&,
                                const webm::SimpleBlock&) override {
    return Commit();
  }

  webm::Status OnBlockGroupBegin(const webm::ElementMetadata&,
                                 webm::Action* action) override {
    *action = webm::Action::kRead;
    in_group_ = true;
    mores_.clear();
    pending_valid_ = false;
    return Ok();
  }

  webm::Status OnBlockBegin(const webm::ElementMetadata&,
                            const webm::Block& block,
                            webm::Action* action) override {
    return BeginBlock(block, action);
  }

  webm::Status OnBlockGroupEnd(const webm::ElementMetadata&,
                               const webm::BlockGroup& group) override {
    in_group_ = false;
    if (!pending_valid_) return Ok();
    // A Block inside a BlockGroup is a keyframe iff it references nothing.
    pending_.keyframe = group.references.empty();
    if (group.additions.is_present()) {
      const auto& mores = group.additions.value().block_mores;
      if (mores.size() != mores_.size()) {
        return Fail(kErrInvalid, "BlockMore bookkeeping mismatch");
      }
      for (std::size_t i = 0; i < mores.size(); ++i) {
        if (mores[i].value().id.value() == 1 && mores_[i].present()) {
          pending_.alpha = mores_[i];
          break;
        }
      }
    }
    return Commit();
  }

  webm::Status OnFrame(const webm::FrameMetadata& meta, webm::Reader* reader,
                       std::uint64_t* bytes_remaining) override {
    if (pending_valid_) pending_.data = Span{meta.position, meta.size};
    // Skips the bytes without copying them.
    return webm::Callback::OnFrame(meta, reader, bytes_remaining);
  }

  webm::Status error() const { return error_; }
  const char* error_text() const { return error_text_; }

 private:
  template <typename B>
  webm::Status BeginBlock(const B& block, webm::Action* action) {
    pending_valid_ = false;
    pending_ = FrameEntry{};
    if (!out_->has_video || block.track_number != out_->video.number) {
      *action = webm::Action::kSkip;  // Audio and other tracks.
      return Ok();
    }
    if (block.lacing != webm::Lacing::kNone) {
      // Lacing packs several frames into one block, and BlockAdditions apply
      // to the whole block, so alpha would be ambiguous. Nobody laces video.
      return Fail(kErrUnsupported, "laced video block");
    }
    *action = webm::Action::kRead;
    const std::int64_t tc =
        static_cast<std::int64_t>(cluster_timecode_) + block.timecode;
    pending_.pts_ns = tc * static_cast<std::int64_t>(out_->timecode_scale_ns);
    pending_valid_ = true;
    return Ok();
  }

  webm::Status Commit() {
    if (!pending_valid_) return Ok();
    pending_valid_ = false;
    if (!pending_.data.present()) return Fail(kErrInvalid, "empty frame");
    if (out_->frames.size() >= limits_.max_frames) {
      return Fail(kErrLimit, "too many frames");
    }
    out_->frames.push_back(pending_);
    return Ok();
  }

  static webm::Status Ok() { return webm::Status(webm::Status::kOkCompleted); }
  webm::Status Fail(std::int32_t code, const char* why) {
    error_ = webm::Status(code);
    error_text_ = why;
    return error_;
  }

  std::size_t file_size_;
  Limits limits_;
  FrameIndex* out_;
  std::uint64_t elements_ = 0;
  bool saw_ebml_ = false;
  std::uint64_t cluster_timecode_ = 0;
  bool in_group_ = false;
  bool pending_valid_ = false;
  FrameEntry pending_;
  std::vector<Span> mores_;
  webm::Status error_{webm::Status::kOkCompleted};
  const char* error_text_ = "";
};

}  // namespace

FrameIndex IndexWithLibwebm(const std::uint8_t* data, std::size_t size,
                            const Limits& limits) {
  FrameIndex out;
  SpanReader reader(data, size);
  IndexCallback callback(size, limits, &out);
  webm::WebmParser parser;
  const webm::Status s = parser.Feed(&callback, &reader);

  if (s.code == webm::Status::kOkCompleted) {
    out.status = IndexStatus::kOk;
  } else if (s.code == webm::Status::kEndOfFile ||
             s.code == webm::Status::kOkPartial ||
             s.code == webm::Status::kWouldBlock) {
    out.status = IndexStatus::kTruncated;
    out.error = "input ended inside an element";
  } else if (s.code == kErrUnsupported) {
    out.status = IndexStatus::kUnsupported;
    out.error = callback.error_text();
  } else if (s.code == kErrLimit) {
    out.status = IndexStatus::kLimit;
    out.error = callback.error_text();
  } else {
    out.status = IndexStatus::kInvalid;
    out.error = s.code == kErrInvalid
                    ? callback.error_text()
                    : "webm_parser status " + std::to_string(s.code);
  }
  if (out.status == IndexStatus::kOk && !out.has_video) {
    out.status = IndexStatus::kUnsupported;
    out.error = "no VP8/VP9 video track";
  }
  return out;
}

}  // namespace spike
