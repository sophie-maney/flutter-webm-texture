#include "api/anim.h"

#include <cstring>
#include <new>
#include <utility>

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
                       std::unique_ptr<Anim>* out) {
  out->reset();
  if ((width == 0) != (height == 0) || width > limits.max_dimension ||
      height > limits.max_dimension) {
    return WT_ERR_INVALID_ARGUMENT;
  }

  std::unique_ptr<Anim> anim(new (std::nothrow) Anim);
  if (anim == nullptr) return WT_ERR_NO_MEMORY;
  anim->bytes_ = bytes;
  const wt_status probe =
      ProbeSource(bytes, format, limits, &anim->info_, &anim->index_);
  if (probe != WT_OK) return probe;
  const WebmVideoTrack& track = anim->index_.video;
  anim->max_pass_pixels_ = limits.max_total_pixels;

  wt_status s = anim->decoder_.Init(track.codec, track.width, track.height,
                                    track.has_alpha());
  if (s != WT_OK) return s;
  if (width == 0) {
    width = track.width;
    height = track.height;
  }
  s = anim->frame_.Allocate(width, height, track.has_alpha());
  if (s != WT_OK) return s;
  *out = std::move(anim);
  return WT_OK;
}

uint32_t Anim::RandomAccessBefore(uint32_t index) const {
  for (uint32_t i = index + 1; i > 0; --i) {
    if (index_.frames[i - 1].random_access) return i - 1;
  }
  return 0;
}

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
  const wt_status s = decoder_.Decode(SpanView(bytes_, f.data),
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

wt_status Anim::NextFrame(uint8_t* dst, size_t dst_size, uint32_t stride,
                          wt_pixel_format format, wt_frame_info* out_frame) {
  if (out_frame != nullptr) std::memset(out_frame, 0, sizeof(*out_frame));
  if (cursor_ >= info_.frame_count) return WT_END;
  if (CheckPixelBuffer(width(), height(), dst_size, stride) != WT_OK) {
    return WT_ERR_INVALID_ARGUMENT;
  }

  // Catch up from a seek. Errors here are the target frame's problem: the
  // decoder resynchronises at the next keyframe either way.
  while (next_decode_ < cursor_) {
    const wt_status s = DecodeFrame(next_decode_++, /*present=*/false);
    if (s == WT_ERR_NO_MEMORY) return s;
  }
  const uint32_t index = cursor_++;
  next_decode_ = cursor_;
  wt_status s = DecodeFrame(index, /*present=*/true);
  // A packet that showed nothing repeats the previous frame; with none yet
  // there is nothing to show.
  if (s == WT_OK && !have_frame_) s = WT_ERR_DECODE;
  if (s != WT_OK) return s;

  s = YuvaToPremultiplied(frame_.view(), dst, dst_size, stride, format);
  if (s != WT_OK) return s;
  if (out_frame != nullptr) {
    const WebmFrame& f = index_.frames[index];
    int64_t rel_ns;
    if (__builtin_sub_overflow(f.pts_ns, index_.frames[0].pts_ns, &rel_ns)) {
      rel_ns = f.pts_ns < 0 ? INT64_MIN : INT64_MAX;
    }
    out_frame->index = index;
    out_frame->pts_ms = RoundNsToMs(rel_ns);
    const uint32_t ms = ClampToU32(RoundNsToMs(f.duration_ns));
    out_frame->duration_ms = ms == 0 ? 1 : ms;  // duration_ns is > 0.
  }
  return WT_OK;
}

}  // namespace wt
