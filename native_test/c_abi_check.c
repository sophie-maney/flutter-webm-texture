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

_Static_assert(offsetof(wt_cache_stats, file_count) == 0,
               "wt_cache_stats layout");
_Static_assert(offsetof(wt_cache_stats, total_bytes) == 8,
               "wt_cache_stats layout");
_Static_assert(sizeof(wt_cache_stats) == 16, "wt_cache_stats layout");

_Static_assert(offsetof(wt_sched_options, worker_count) == 0,
               "wt_sched_options layout");
_Static_assert(offsetof(wt_sched_options, max_fps) == 4,
               "wt_sched_options layout");
_Static_assert(offsetof(wt_sched_options, max_concurrent) == 8,
               "wt_sched_options layout");
_Static_assert(sizeof(wt_sched_options) == 12, "wt_sched_options layout");

_Static_assert(offsetof(wt_sched_hooks, context) == 0, "wt_sched_hooks layout");
_Static_assert(offsetof(wt_sched_hooks, thread_start) == sizeof(void*),
               "wt_sched_hooks layout");
_Static_assert(offsetof(wt_sched_hooks, thread_stop) == 2 * sizeof(void*),
               "wt_sched_hooks layout");
_Static_assert(sizeof(wt_sched_hooks) == 3 * sizeof(void*),
               "wt_sched_hooks layout");

_Static_assert(offsetof(wt_sched_stats, players) == 0, "wt_sched_stats layout");
_Static_assert(offsetof(wt_sched_stats, active) == 4, "wt_sched_stats layout");
_Static_assert(offsetof(wt_sched_stats, running) == 8, "wt_sched_stats layout");
_Static_assert(offsetof(wt_sched_stats, workers) == 12,
               "wt_sched_stats layout");
_Static_assert(offsetof(wt_sched_stats, ticks) == 16, "wt_sched_stats layout");
_Static_assert(offsetof(wt_sched_stats, wakeups) == 24,
               "wt_sched_stats layout");
_Static_assert(offsetof(wt_sched_stats, frames_drawn) == 32,
               "wt_sched_stats layout");
_Static_assert(offsetof(wt_sched_stats, frames_skipped) == 40,
               "wt_sched_stats layout");
_Static_assert(sizeof(wt_sched_stats) == 48, "wt_sched_stats layout");

_Static_assert(offsetof(wt_buffer, pixels) == 0, "wt_buffer layout");
_Static_assert(offsetof(wt_buffer, size) == sizeof(void*), "wt_buffer layout");
_Static_assert(offsetof(wt_buffer, stride) == 2 * sizeof(void*),
               "wt_buffer layout");
_Static_assert(offsetof(wt_buffer, format) == 2 * sizeof(void*) + 4,
               "wt_buffer layout");
_Static_assert(sizeof(wt_buffer) == 2 * sizeof(void*) + 8, "wt_buffer layout");

_Static_assert(offsetof(wt_sink, context) == 0, "wt_sink layout");
_Static_assert(offsetof(wt_sink, acquire) == sizeof(void*), "wt_sink layout");
_Static_assert(offsetof(wt_sink, release) == 2 * sizeof(void*),
               "wt_sink layout");
_Static_assert(offsetof(wt_sink, error) == 3 * sizeof(void*), "wt_sink layout");
_Static_assert(sizeof(wt_sink) == 4 * sizeof(void*), "wt_sink layout");

_Static_assert(offsetof(wt_atlas_options, width) == 0,
               "wt_atlas_options layout");
_Static_assert(offsetof(wt_atlas_options, max_height) == 4,
               "wt_atlas_options layout");
_Static_assert(offsetof(wt_atlas_options, gutter) == 8,
               "wt_atlas_options layout");
_Static_assert(offsetof(wt_atlas_options, max_pages) == 12,
               "wt_atlas_options layout");
_Static_assert(offsetof(wt_atlas_options, format) == 16,
               "wt_atlas_options layout");
_Static_assert(sizeof(wt_atlas_options) == 20, "wt_atlas_options layout");

_Static_assert(offsetof(wt_atlas_slot, page) == 0, "wt_atlas_slot layout");
_Static_assert(offsetof(wt_atlas_slot, x) == 4, "wt_atlas_slot layout");
_Static_assert(offsetof(wt_atlas_slot, height) == 16, "wt_atlas_slot layout");
_Static_assert(offsetof(wt_atlas_slot, outer_x) == 20, "wt_atlas_slot layout");
_Static_assert(offsetof(wt_atlas_slot, outer_height) == 32,
               "wt_atlas_slot layout");
_Static_assert(sizeof(wt_atlas_slot) == 36, "wt_atlas_slot layout");

_Static_assert(offsetof(wt_atlas_page_info, width) == 0,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, used_height) == 4,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, wanted_height) == 8,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, height) == 12,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, has_target) == 16,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, live_slots) == 20,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, buffers) == 24,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, buffer_stride) == 28,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, buffer_bytes) == 32,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, cpu_bytes) == 40,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, presents) == 48,
               "wt_atlas_page_info layout");
_Static_assert(offsetof(wt_atlas_page_info, present_failures) == 56,
               "wt_atlas_page_info layout");
_Static_assert(sizeof(wt_atlas_page_info) == 64, "wt_atlas_page_info layout");

_Static_assert(offsetof(wt_atlas_target, context) == 0,
               "wt_atlas_target layout");
_Static_assert(offsetof(wt_atlas_target, acquire) == sizeof(void*),
               "wt_atlas_target layout");
_Static_assert(offsetof(wt_atlas_target, present) == 2 * sizeof(void*),
               "wt_atlas_target layout");
_Static_assert(sizeof(wt_atlas_target) == 3 * sizeof(void*),
               "wt_atlas_target layout");

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
  if (wt_cache_get_stats(NULL, NULL) != WT_ERR_INVALID_ARGUMENT) return 6;
  if (wt_cache_evict(NULL, NULL) != WT_ERR_INVALID_ARGUMENT) return 7;
  if (wt_cache_clear(NULL, NULL) != WT_ERR_INVALID_ARGUMENT) return 8;
  if (wt_cache_file_path(NULL, NULL, 0, 0, NULL, 0) !=
      WT_ERR_INVALID_ARGUMENT) {
    return 9;
  }
  if (wt_cache_get_budget() == 0) return 10;
  {
    wt_sched_options options;
    wt_sched_options_init_default(&options);
    if (options.worker_count == 0 || options.max_fps == 0) return 11;
    if (wt_sched_create(&options, NULL, NULL) != WT_ERR_INVALID_ARGUMENT) {
      return 12;
    }
    if (wt_player_create(NULL, NULL, false, NULL, NULL) !=
        WT_ERR_INVALID_ARGUMENT) {
      return 13;
    }
    wt_sched_destroy(NULL);
    wt_player_destroy(NULL);
  }
  {
    wt_atlas_options options;
    wt_atlas_page_info info;
    wt_atlas_slot slot;
    wt_atlas_options_init_default(&options);
    if (options.width == 0 || options.max_pages == 0) return 14;
    if (wt_atlas_create(NULL, &options, NULL) != WT_ERR_INVALID_ARGUMENT) {
      return 15;
    }
    if (wt_atlas_attach(NULL, NULL, &slot) != WT_ERR_INVALID_ARGUMENT) {
      return 16;
    }
    if (wt_atlas_get_page(NULL, 0, &info) != WT_ERR_INVALID_ARGUMENT) {
      return 17;
    }
    if (wt_atlas_page_count(NULL) != 0) return 18;
    wt_atlas_destroy(NULL);
  }
  return 0;
}
