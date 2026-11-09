/*
 * Compiles wt_api.h as strict C11 (-Wpedantic -Werror) and pins the layout of
 * every struct in it. ffigen generates Dart structs from this header, so a
 * layout change must also bump WT_API_VERSION and regenerate the bindings.
 */
#include <stddef.h>

#include "wt_api.h"

_Static_assert(sizeof(wt_status) == 4, "wt_status is int32_t");
_Static_assert(sizeof(wt_format) == 4, "wt_format is int32_t");
_Static_assert(sizeof(wt_codec) == 4, "wt_codec is int32_t");
_Static_assert(sizeof(wt_pixel_format) == 4, "wt_pixel_format is int32_t");

_Static_assert(offsetof(wt_limits, max_bytes) == 0, "wt_limits layout");
_Static_assert(offsetof(wt_limits, max_total_pixels) == 8, "wt_limits layout");
_Static_assert(offsetof(wt_limits, max_dimension) == 16, "wt_limits layout");
_Static_assert(offsetof(wt_limits, max_frames) == 20, "wt_limits layout");
_Static_assert(offsetof(wt_limits, max_duration_ms) == 24, "wt_limits layout");
_Static_assert(sizeof(wt_limits) == 32, "wt_limits layout");

_Static_assert(offsetof(wt_info, format) == 0, "wt_info layout");
_Static_assert(offsetof(wt_info, codec) == 4, "wt_info layout");
_Static_assert(offsetof(wt_info, width) == 8, "wt_info layout");
_Static_assert(offsetof(wt_info, height) == 12, "wt_info layout");
_Static_assert(offsetof(wt_info, frame_count) == 16, "wt_info layout");
_Static_assert(offsetof(wt_info, duration_ms) == 20, "wt_info layout");
_Static_assert(offsetof(wt_info, fps) == 24, "wt_info layout");
_Static_assert(offsetof(wt_info, has_alpha) == 32, "wt_info layout");
_Static_assert(offsetof(wt_info, has_audio) == 33, "wt_info layout");
_Static_assert(offsetof(wt_info, truncated) == 34, "wt_info layout");
_Static_assert(sizeof(wt_info) == 40, "wt_info layout");

_Static_assert(offsetof(wt_frame_info, index) == 0, "wt_frame_info layout");
_Static_assert(offsetof(wt_frame_info, duration_ms) == 4,
               "wt_frame_info layout");
_Static_assert(offsetof(wt_frame_info, pts_ms) == 8, "wt_frame_info layout");
_Static_assert(sizeof(wt_frame_info) == 16, "wt_frame_info layout");

/* Called from api_test.cc: proves the functions link and work from C. Only
   checks behaviour that stays true once the stubs are implemented. */
int wt_c_abi_check(void) {
  wt_limits limits;
  wt_info info;
  static const uint8_t kByte = 0;

  wt_limits_init_default(&limits);
  if (limits.max_dimension == 0) return 1;
  if (wt_api_version() != WT_API_VERSION) return 2;
  if (wt_status_string(WT_OK) == NULL) return 3;
  if (wt_source_create_memory(&kByte, 1, &limits, NULL) !=
      WT_ERR_INVALID_ARGUMENT) {
    return 4;
  }
  if (wt_probe(NULL, &info) != WT_ERR_INVALID_ARGUMENT) return 5;
  wt_source_destroy(NULL);
  wt_anim_destroy(NULL);
  return 0;
}
