#include "demux/webp.h"

#include <climits>
#include <cstring>
#include <memory>

#include "convert/yuva.h"
#include "webp/demux.h"

namespace wt {

namespace {

constexpr uint32_t kMinDurationMs = 10;  // At or below this, 100 ms.
constexpr uint32_t kDefaultDurationMs = 100;

struct DemuxDeleter {
  void operator()(WebPDemuxer* d) const { WebPDemuxDelete(d); }
};
using DemuxPtr = std::unique_ptr<WebPDemuxer, DemuxDeleter>;

// Checks the limits that only need the canvas and the frame count. Called as
// early as possible so a hostile file costs little.
wt_status CheckCanvas(uint32_t width, uint32_t height, uint32_t frames,
                      const wt_limits& limits) {
  if (width == 0 || height == 0) return WT_ERR_INVALID_DATA;
  if (width > limits.max_dimension || height > limits.max_dimension ||
      width > YuvaImage::kMaxDimension || height > YuvaImage::kMaxDimension) {
    return WT_ERR_LIMIT;
  }
  if (frames > limits.max_frames) return WT_ERR_LIMIT;
  if (uint64_t{width} * height >
      limits.max_total_pixels / (frames ? frames : 1)) {
    return WT_ERR_LIMIT;
  }
  return WT_OK;
}

// Reads the facts every frame iterator gives, for frames 1..count, stopping at
// the first incomplete one. Returns how many were complete; *end is the offset
// just past the last complete frame's payload.
wt_status ReadFrames(const WebPDemuxer* dmux, ByteView bytes, uint32_t count,
                     const wt_limits& limits, WebpIndex* out,
                     uint32_t* complete, size_t* end) {
  uint64_t total_ms = 0;
  *complete = 0;
  *end = 0;
  for (uint32_t n = 1; n <= count; ++n) {
    WebPIterator it;
    if (!WebPDemuxGetFrame(dmux, static_cast<int>(n), &it)) break;
    const bool done = it.complete != 0;
    const bool alpha = it.has_alpha != 0;
    const uint32_t ms = WebpFrameDurationMs(it.duration);
    size_t frame_end = 0;
    if (done && it.fragment.bytes >= bytes.data &&
        it.fragment.bytes <= bytes.data + bytes.size &&
        it.fragment.size <=
            static_cast<size_t>(bytes.data + bytes.size - it.fragment.bytes)) {
      frame_end = static_cast<size_t>(it.fragment.bytes - bytes.data) +
                  it.fragment.size;
    }
    WebPDemuxReleaseIterator(&it);
    if (!done) break;
    total_ms += ms;
    if (total_ms > limits.max_duration_ms) return WT_ERR_LIMIT;
    out->has_alpha = out->has_alpha || alpha;
    out->duration_ms.push_back(ms);
    *complete = n;
    *end = frame_end;
  }
  return WT_OK;
}

}  // namespace

uint32_t WebpFrameDurationMs(int duration) {
  if (duration <= static_cast<int>(kMinDurationMs)) return kDefaultDurationMs;
  return static_cast<uint32_t>(duration);
}

wt_status DemuxWebp(ByteView bytes, const wt_limits& limits, WebpIndex* out) {
  *out = WebpIndex();
  if ((bytes.data == nullptr && bytes.size != 0) || limits.max_frames == 0 ||
      limits.max_dimension == 0 || limits.max_total_pixels == 0 ||
      limits.max_duration_ms == 0) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  WebPData data;
  WebPDataInit(&data);
  data.bytes = bytes.data;
  data.size = bytes.size;
  WebPDemuxState state = WEBP_DEMUX_PARSE_ERROR;
  DemuxPtr dmux(WebPDemuxPartial(&data, &state));
  if (dmux == nullptr || state == WEBP_DEMUX_PARSE_ERROR) {
    return WT_ERR_INVALID_DATA;
  }
  if (state == WEBP_DEMUX_PARSING_HEADER) return WT_ERR_TRUNCATED;

  const uint32_t width = WebPDemuxGetI(dmux.get(), WEBP_FF_CANVAS_WIDTH);
  const uint32_t height = WebPDemuxGetI(dmux.get(), WEBP_FF_CANVAS_HEIGHT);
  const uint32_t count = WebPDemuxGetI(dmux.get(), WEBP_FF_FRAME_COUNT);
  wt_status s = CheckCanvas(width, height, count, limits);
  if (s != WT_OK) return s;
  out->width = width;
  out->height = height;
  out->has_alpha =
      (WebPDemuxGetI(dmux.get(), WEBP_FF_FORMAT_FLAGS) & ALPHA_FLAG) != 0;

  uint32_t complete = 0;
  size_t end = 0;
  s = ReadFrames(dmux.get(), bytes, count, limits, out, &complete, &end);
  if (s != WT_OK) {
    *out = WebpIndex();
    return s;
  }
  if (complete == 0) {
    *out = WebpIndex();
    return state == WEBP_DEMUX_DONE ? WT_ERR_INVALID_DATA : WT_ERR_TRUNCATED;
  }
  if (state == WEBP_DEMUX_DONE && complete == count) return WT_OK;

  // Cut inside a frame, or a frame that never completed: decode the complete
  // frames from a copy that ends after the last of them.
  if (end % 2 != 0) ++end;  // RIFF chunks are padded to even sizes.
  if (end < 12 || end > bytes.size) {
    *out = WebpIndex();
    return WT_ERR_TRUNCATED;
  }
  out->truncated = true;
  out->prefix.assign(bytes.data, bytes.data + end);
  const uint32_t riff_size = static_cast<uint32_t>(end - 8);
  for (int i = 0; i < 4; ++i) {
    out->prefix[4 + static_cast<size_t>(i)] =
        static_cast<uint8_t>(riff_size >> (8 * i));
  }
  // The copy must be a whole file with exactly the frames we counted.
  WebPData prefix;
  WebPDataInit(&prefix);
  prefix.bytes = out->prefix.data();
  prefix.size = out->prefix.size();
  DemuxPtr whole(WebPDemux(&prefix));
  if (whole == nullptr ||
      WebPDemuxGetI(whole.get(), WEBP_FF_FRAME_COUNT) != complete) {
    *out = WebpIndex();
    return WT_ERR_TRUNCATED;
  }
  out->duration_ms.resize(complete);
  return WT_OK;
}

}  // namespace wt
