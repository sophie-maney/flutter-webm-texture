// Implementation of the C ABI in wt_api.h.
//
// T1.1: every function checks its arguments; those whose work belongs to a
// later task then return WT_ERR_UNIMPLEMENTED. Nothing here may throw: the
// core builds with -fno-exceptions and every failure is a wt_status.

#include "api/wt_api.h"

#include <cstdint>
#include <cstring>

// The handles are empty until T1.2 (sources) and T1.4 (animations) give them
// state. Defined here so the destroy functions are already correct.
struct wt_source {};
struct wt_anim {};

namespace {

constexpr uint64_t kDefaultMaxBytes = uint64_t{2} << 20;
constexpr uint64_t kDefaultMaxTotalPixels = uint64_t{160} << 20;
constexpr uint32_t kDefaultMaxDimension = 1024;
constexpr uint32_t kDefaultMaxFrames = 600;
constexpr uint32_t kDefaultMaxDurationMs = 10000;

// NULL means "use the defaults". Zero fields are rejected rather than read as
// "unlimited", so a zero-initialised struct can't switch the limits off.
bool LimitsValid(const wt_limits* limits) {
  if (limits == nullptr) return true;
  return limits->max_bytes != 0 && limits->max_total_pixels != 0 &&
         limits->max_dimension != 0 && limits->max_frames != 0 &&
         limits->max_duration_ms != 0;
}

bool PixelFormatValid(wt_pixel_format format) {
  return format == WT_PIXEL_FORMAT_RGBA8888 ||
         format == WT_PIXEL_FORMAT_BGRA8888;
}

}  // namespace

extern "C" {

int32_t wt_api_version(void) { return WT_API_VERSION; }

const char* wt_status_string(wt_status status) {
  switch (status) {
    case WT_OK:
      return "WT_OK";
    case WT_END:
      return "WT_END";
    case WT_ERR_INVALID_ARGUMENT:
      return "WT_ERR_INVALID_ARGUMENT";
    case WT_ERR_UNIMPLEMENTED:
      return "WT_ERR_UNIMPLEMENTED";
    case WT_ERR_INVALID_DATA:
      return "WT_ERR_INVALID_DATA";
    case WT_ERR_UNSUPPORTED:
      return "WT_ERR_UNSUPPORTED";
    case WT_ERR_TRUNCATED:
      return "WT_ERR_TRUNCATED";
    case WT_ERR_LIMIT:
      return "WT_ERR_LIMIT";
    case WT_ERR_NO_MEMORY:
      return "WT_ERR_NO_MEMORY";
    case WT_ERR_IO:
      return "WT_ERR_IO";
    case WT_ERR_DECODE:
      return "WT_ERR_DECODE";
    case WT_ERR_INTERNAL:
      return "WT_ERR_INTERNAL";
  }
  return "WT_ERR_?";
}

void wt_limits_init_default(wt_limits* out) {
  if (out == nullptr) return;
  std::memset(out, 0, sizeof(*out));
  out->max_bytes = kDefaultMaxBytes;
  out->max_total_pixels = kDefaultMaxTotalPixels;
  out->max_dimension = kDefaultMaxDimension;
  out->max_frames = kDefaultMaxFrames;
  out->max_duration_ms = kDefaultMaxDurationMs;
}

wt_status wt_source_create_memory(const uint8_t* data, size_t size,
                                  const wt_limits* limits,
                                  wt_source** out_source) {
  if (out_source == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out_source = nullptr;
  if ((data == nullptr && size != 0) || !LimitsValid(limits)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  return WT_ERR_UNIMPLEMENTED;  // T1.2
}

wt_status wt_source_create_file(const char* path, const wt_limits* limits,
                                wt_source** out_source) {
  if (out_source == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out_source = nullptr;
  if (path == nullptr || path[0] == '\0' || !LimitsValid(limits)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  return WT_ERR_UNIMPLEMENTED;  // T1.2
}

void wt_source_destroy(wt_source* source) { delete source; }

wt_status wt_probe(wt_source* source, wt_info* out_info) {
  if (out_info == nullptr) return WT_ERR_INVALID_ARGUMENT;
  std::memset(out_info, 0, sizeof(*out_info));
  if (source == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return WT_ERR_UNIMPLEMENTED;  // T1.5
}

wt_status wt_anim_create(wt_source* source, uint32_t width, uint32_t height,
                         wt_anim** out_anim) {
  if (out_anim == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out_anim = nullptr;
  if (source == nullptr || ((width == 0) != (height == 0))) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  return WT_ERR_UNIMPLEMENTED;  // T1.4
}

void wt_anim_destroy(wt_anim* anim) { delete anim; }

wt_status wt_anim_get_info(const wt_anim* anim, wt_info* out_info,
                           uint32_t* out_width, uint32_t* out_height) {
  if (out_info != nullptr) std::memset(out_info, 0, sizeof(*out_info));
  if (out_width != nullptr) *out_width = 0;
  if (out_height != nullptr) *out_height = 0;
  if (anim == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return WT_ERR_UNIMPLEMENTED;  // T1.4
}

wt_status wt_anim_seek(wt_anim* anim, uint32_t frame_index) {
  (void)frame_index;
  if (anim == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return WT_ERR_UNIMPLEMENTED;  // T1.4
}

wt_status wt_anim_next_frame(wt_anim* anim, uint8_t* dst, size_t dst_size,
                             uint32_t stride, wt_pixel_format pixel_format,
                             wt_frame_info* out_frame) {
  if (out_frame != nullptr) std::memset(out_frame, 0, sizeof(*out_frame));
  if (anim == nullptr || dst == nullptr || dst_size == 0 || stride == 0 ||
      !PixelFormatValid(pixel_format)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  return WT_ERR_UNIMPLEMENTED;  // T1.4
}

}  // extern "C"
