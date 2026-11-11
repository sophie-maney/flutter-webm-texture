#include "api/anim.h"

#include <unistd.h>

#include <cstring>
#include <new>
#include <utility>

#include "cache/crc32.h"
#include "cache/store.h"

namespace wt {

namespace {

constexpr int64_t kNsPerMs = 1000000;

// ns -> ms, rounded half away from zero. No overflow for any int64.
int64_t RoundNsToMs(int64_t ns) {
  int64_t ms = ns / kNsPerMs;
  const int64_t rem = ns % kNsPerMs;
  if (rem >= kNsPerMs / 2) ++ms;
  if (rem <= -kNsPerMs / 2) --ms;
  return ms;
}

uint32_t ClampToU32(int64_t value) {
  if (value < 0) return 0;
  if (static_cast<uint64_t>(value) > UINT32_MAX) return UINT32_MAX;
  return static_cast<uint32_t>(value);
}

ByteView SpanView(ByteView bytes, Span span) {
  // The demuxer only records spans inside `bytes`.
  if (span.empty()) return ByteView{};
  return ByteView{bytes.data + span.offset, static_cast<size_t>(span.size)};
}

}  // namespace

void WebmInfo(const WebmIndex& index, wt_info* out) {
  std::memset(out, 0, sizeof(*out));
  out->format = WT_FORMAT_WEBM;
  out->codec = index.video.codec;
  out->width = index.video.width;
  out->height = index.video.height;
  out->frame_count = static_cast<uint32_t>(index.frames.size());
  int64_t total_ns = 0;
  for (const WebmFrame& f : index.frames) {
    if (__builtin_add_overflow(total_ns, f.duration_ns, &total_ns)) {
      total_ns = INT64_MAX;
      break;
    }
  }
  out->duration_ms = ClampToU32(RoundNsToMs(total_ns));
  out->fps = out->duration_ms == 0
                 ? 0.0
                 : out->frame_count * 1000.0 / out->duration_ms;
  out->has_alpha = index.video.has_alpha();
  out->has_audio = index.has_audio;
  out->truncated = index.status == WT_ERR_TRUNCATED;
}

wt_status ProbeSource(ByteView bytes, wt_format format, const wt_limits& limits,
                      wt_info* out_info, WebmIndex* index) {
  std::memset(out_info, 0, sizeof(*out_info));
  switch (format) {
    case WT_FORMAT_WEBM:
      break;
    case WT_FORMAT_WEBP:
    case WT_FORMAT_GIF:
      return WT_ERR_UNSUPPORTED;  // Phase 5.
    default:
      return WT_ERR_INVALID_DATA;
  }
  WebmIndex local;
  WebmIndex* idx = index != nullptr ? index : &local;
  const wt_status demux = DemuxWebm(bytes, limits, idx);
  // A truncated file plays the complete frames before the cut (ADR 0008).
  if (demux != WT_OK && !(demux == WT_ERR_TRUNCATED && !idx->frames.empty())) {
    return demux;
  }
  if (idx->frames.empty() || idx->video.width == 0 || idx->video.height == 0) {
    return WT_ERR_INTERNAL;  // The demuxer promises a playable track.
  }
  WebmInfo(*idx, out_info);
  return WT_OK;
}

Anim::~Anim() = default;

wt_status Anim::Create(ByteView bytes, wt_format format,
                       const wt_limits& limits, uint32_t width, uint32_t height,
                       const char* cache_path, std::unique_ptr<Anim>* out) {
  out->reset();
  if ((width == 0) != (height == 0) || width > limits.max_dimension ||
      height > limits.max_dimension) {
    return WT_ERR_INVALID_ARGUMENT;
  }

  std::unique_ptr<Anim> anim(new (std::nothrow) Anim);
  if (anim == nullptr) return WT_ERR_NO_MEMORY;
  anim->bytes_ = bytes;
  anim->limits_ = limits;
  const wt_status probe =
      ProbeSource(bytes, format, limits, &anim->info_, &anim->index_);
  if (probe != WT_OK) return probe;
  const WebmVideoTrack& track = anim->index_.video;
  anim->max_pass_pixels_ = limits.max_total_pixels;

  if (width == 0) {
    width = track.width;
    height = track.height;
  }
  wt_status s = anim->frame_.Allocate(width, height, track.has_alpha());
  if (s != WT_OK) return s;

  if (cache_path != nullptr && cache_path[0] != '\0') {
    anim->cache_path_ = cache_path;
    anim->source_crc_ = Crc32(0, bytes.data, bytes.size);
    anim->OpenCache();
  }
  // The cache makes the decoder unnecessary until something goes wrong.
  if (anim->reader_ == nullptr) {
    s = anim->EnsureDecoder(0);
    if (s != WT_OK) return s;
  }
  *out = std::move(anim);
  return WT_OK;
}

Anim::CacheMode Anim::cache_mode() const {
  if (reader_ != nullptr) return CacheMode::kReading;
  if (writer_ != nullptr) return CacheMode::kWriting;
  return cache_path_.empty() ? CacheMode::kNone : CacheMode::kPending;
}

wt_status Anim::EnsureDecoder(uint32_t target) {
  if (decoder_ != nullptr) return WT_OK;
  std::unique_ptr<VpxDecoder> decoder(new (std::nothrow) VpxDecoder);
  if (decoder == nullptr) return WT_ERR_NO_MEMORY;
  const WebmVideoTrack& track = index_.video;
  const wt_status s =
      decoder->Init(track.codec, track.width, track.height, track.has_alpha());
  if (s != WT_OK) return s;
  decoder_ = std::move(decoder);
  // A fresh decoder starts at a random-access frame (see Seek).
  decoder_healthy_ = true;
  next_decode_ = RandomAccessBefore(target);
  return WT_OK;
}

uint32_t Anim::RandomAccessBefore(uint32_t index) const {
  for (uint32_t i = index + 1; i > 0; --i) {
    if (index_.frames[i - 1].random_access) return i - 1;
  }
  return 0;
}

uint32_t Anim::FrameDurationMs(uint32_t index) const {
  const uint32_t ms = ClampToU32(RoundNsToMs(index_.frames[index].duration_ns));
  return ms == 0 ? 1 : ms;  // duration_ns is > 0.
}

int64_t Anim::FramePtsMs(uint32_t index) const {
  const int64_t pts = index_.frames[index].pts_ns;
  int64_t rel_ns;
  if (__builtin_sub_overflow(pts, index_.frames[0].pts_ns, &rel_ns)) {
    rel_ns = pts < 0 ? INT64_MIN : INT64_MAX;
  }
  return RoundNsToMs(rel_ns);
}

// --- Frame cache
// --------------------------------------------------------------

WscParams Anim::CacheParams() const {
  WscParams p;
  p.width = frame_.width();
  p.height = frame_.height();
  p.has_alpha = frame_.has_alpha();
  p.frame_count = info_.frame_count;
  p.source_size = bytes_.size;
  p.source_crc = source_crc_;
  return p;
}

void Anim::OpenCache() {
  const WscParams params = CacheParams();
  std::unique_ptr<ByteSource> file;
  wt_status s =
      ByteSource::OpenFile(cache_path_.c_str(), WscMaxFileSize(params), &file);
  if (s != WT_OK) {
    // WT_ERR_IO is usually the normal miss (no file). A file too big to be a
    // valid cache is junk in our name, and it goes.
    if (s == WT_ERR_LIMIT) unlink(cache_path_.c_str());
    return;
  }
  std::unique_ptr<WscReader> reader;
  s = WscReader::Open(std::move(file), params, limits_, &reader);
  // The timing must match the source's too: the cache stands in for it.
  for (uint32_t i = 0; s == WT_OK && i < info_.frame_count; ++i) {
    const WscFrame& f = reader->frame(i);
    if (f.duration_ms != FrameDurationMs(i) || f.pts_ms != FramePtsMs(i)) {
      s = WT_ERR_INVALID_DATA;
    }
  }
  if (s != WT_OK) {
    if (s != WT_ERR_NO_MEMORY) unlink(cache_path_.c_str());
    return;
  }
  reader_ = std::move(reader);
  TouchCacheFile(cache_path_.c_str());  // For LRU eviction (cache/store.h).
}

void Anim::DropCache() {
  reader_.reset();
  unlink(cache_path_.c_str());
}

void Anim::DisableCache() {
  writer_.reset();
  cache_path_.clear();
}

void Anim::WriteCache(uint32_t index) {
  if (cache_path_.empty()) return;
  // A seek broke the pass: drop what was written. The next pass from frame 0
  // (maybe this one) starts over.
  if (writer_ != nullptr && index != writer_->frames_written()) {
    writer_.reset();
  }
  if (writer_ == nullptr) {
    if (index != 0) return;
    const wt_status s = WscWriter::Create(cache_path_.c_str(), CacheParams(),
                                          frame_.color(), &writer_);
    // Most likely a missing or read-only directory: don't retry every loop.
    if (s != WT_OK) return DisableCache();
  }
  wt_status s =
      writer_->Append(frame_.view(), FrameDurationMs(index), FramePtsMs(index));
  if (s == WT_OK && writer_->full()) s = writer_->Finish();
  if (s != WT_OK) return DisableCache();  // A full disk stays full.
  if (!writer_->full()) return;
  writer_.reset();
  // Later loops read the file instead of decoding.
  OpenCache();
  if (reader_ != nullptr) decoder_.reset();
  // Mapped first, so even if the budget evicts our own file (one larger than
  // the budget) the mapping keeps serving this animation's loops.
  EvictAfterWrite(cache_path_);
}

// --- Playback
// -------------------------------------------------------------------

wt_status Anim::Seek(uint32_t frame_index) {
  if (frame_index >= info_.frame_count) return WT_ERR_INVALID_ARGUMENT;
  // Keep decoding forward when the decoder is healthy and already between
  // the random-access point and the target; otherwise restart at that point.
  // Restarting needs no decoder reset: it begins on keyframes in both
  // streams (spike A, E1c).
  const uint32_t start = RandomAccessBefore(frame_index);
  if (!decoder_healthy_ || next_decode_ < start || next_decode_ > frame_index) {
    next_decode_ = start;
  }
  cursor_ = frame_index;
  pass_pixels_ = 0;
  return WT_OK;
}

wt_status Anim::DecodeFrame(uint32_t index, bool present) {
  const WebmFrame& f = index_.frames[index];
  pass_pixels_ += uint64_t{info_.width} * info_.height;
  if (pass_pixels_ > max_pass_pixels_) return WT_ERR_LIMIT;
  VpxDecoder::Output out;
  const wt_status s = decoder_->Decode(SpanView(bytes_, f.data),
                                       SpanView(bytes_, f.alpha), &out);
  decoder_healthy_ = s == WT_OK;
  if (s != WT_OK || !present || !out.shown) return s;
  wt_status scaled = ScaleColor(out.view, &frame_);
  if (scaled == WT_OK && out.alpha_fresh)
    scaled = ScaleAlpha(out.view, &frame_);
  if (scaled != WT_OK) return scaled;
  have_frame_ = true;
  return WT_OK;
}

wt_status Anim::DecodeAt(uint32_t index) {
  wt_status s = EnsureDecoder(index);
  if (s != WT_OK) return s;
  // Catch up from a seek. Errors here are the target frame's problem: the
  // decoder resynchronises at the next keyframe either way.
  while (next_decode_ < index) {
    s = DecodeFrame(next_decode_++, /*present=*/false);
    if (s == WT_ERR_NO_MEMORY) return s;
  }
  next_decode_ = index + 1;
  s = DecodeFrame(index, /*present=*/true);
  // A packet that showed nothing repeats the previous frame; with none yet
  // there is nothing to show.
  if (s == WT_OK && !have_frame_) s = WT_ERR_DECODE;
  if (s != WT_OK) {
    // A file that doesn't decode cleanly isn't worth caching.
    DisableCache();
    return s;
  }
  WriteCache(index);
  return WT_OK;
}

wt_status Anim::Advance(bool need_pixels, wt_frame_info* out_frame) {
  const uint32_t index = cursor_++;
  wt_status s = WT_ERR_INTERNAL;
  if (reader_ != nullptr) {
    // Every cached frame is complete on its own, so a skip reads nothing.
    s = need_pixels ? reader_->ReadFrame(index, &frame_) : WT_OK;
    if (s == WT_OK) {
      have_frame_ = need_pixels || have_frame_;
    } else {
      // A damaged frame: delete the file and decode from here on. frame_
      // holds a partial frame, so it can't stand in for a hidden packet.
      DropCache();
      have_frame_ = false;
      pass_pixels_ = 0;
    }
  }
  if (reader_ == nullptr) s = DecodeAt(index);
  if (s != WT_OK) return s;
  if (out_frame != nullptr) {
    out_frame->index = index;
    out_frame->pts_ms = FramePtsMs(index);
    out_frame->duration_ms = FrameDurationMs(index);
  }
  return WT_OK;
}

wt_status Anim::NextFrame(uint8_t* dst, size_t dst_size, uint32_t stride,
                          wt_pixel_format format, wt_frame_info* out_frame) {
  if (out_frame != nullptr) std::memset(out_frame, 0, sizeof(*out_frame));
  if (cursor_ >= info_.frame_count) return WT_END;
  if (CheckPixelBuffer(width(), height(), dst_size, stride) != WT_OK) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  wt_frame_info frame{};
  wt_status s = Advance(/*need_pixels=*/true, &frame);
  if (s != WT_OK) return s;
  s = YuvaToPremultiplied(frame_.view(), dst, dst_size, stride, format);
  if (s != WT_OK) return s;
  if (out_frame != nullptr) *out_frame = frame;
  return WT_OK;
}

wt_status Anim::SkipFrame(wt_frame_info* out_frame) {
  if (out_frame != nullptr) std::memset(out_frame, 0, sizeof(*out_frame));
  if (cursor_ >= info_.frame_count) return WT_END;
  return Advance(/*need_pixels=*/false, out_frame);
}

}  // namespace wt
