// The atlas part of the C ABI in wt_api.h (wt_atlas_*): a thin layer over
// wt::Atlas. wt_atlas is never defined; the handle is a wt::Atlas pointer.

#include <cstring>
#include <memory>

#include "api/wt_api.h"
#include "atlas/compositor.h"
#include "sched/scheduler.h"

namespace {

wt::Atlas* Impl(wt_atlas* a) { return reinterpret_cast<wt::Atlas*>(a); }

}  // namespace

extern "C" {

void wt_atlas_options_init_default(wt_atlas_options* out) {
  if (out == nullptr) return;
  std::memset(out, 0, sizeof(*out));
  wt::Atlas::DefaultOptions(out);
}

wt_status wt_atlas_create(wt_sched* sched, const wt_atlas_options* options,
                          wt_atlas** out_atlas) {
  if (out_atlas == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out_atlas = nullptr;
  if (sched == nullptr) return WT_ERR_INVALID_ARGUMENT;
  wt_atlas_options resolved;
  if (options != nullptr) {
    resolved = *options;
  } else {
    wt_atlas_options_init_default(&resolved);
  }
  std::unique_ptr<wt::Atlas> atlas;
  const wt_status s = wt::Atlas::Create(reinterpret_cast<wt::Scheduler*>(sched),
                                        resolved, &atlas);
  if (s != WT_OK) return s;
  *out_atlas = reinterpret_cast<wt_atlas*>(atlas.release());
  return WT_OK;
}

void wt_atlas_destroy(wt_atlas* atlas) { delete Impl(atlas); }

wt_status wt_atlas_attach(wt_atlas* atlas, wt_player* player,
                          wt_atlas_slot* out_slot) {
  if (out_slot == nullptr) return WT_ERR_INVALID_ARGUMENT;
  std::memset(out_slot, 0, sizeof(*out_slot));
  if (atlas == nullptr || player == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return Impl(atlas)->Attach(reinterpret_cast<wt::Player*>(player), out_slot);
}

wt_status wt_atlas_detach(wt_atlas* atlas, wt_player* player) {
  if (atlas == nullptr || player == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return Impl(atlas)->Detach(reinterpret_cast<wt::Player*>(player));
}

uint32_t wt_atlas_page_count(wt_atlas* atlas) {
  return atlas == nullptr ? 0 : Impl(atlas)->page_count();
}

wt_status wt_atlas_get_page(wt_atlas* atlas, uint32_t page,
                            wt_atlas_page_info* out_info) {
  if (out_info == nullptr) return WT_ERR_INVALID_ARGUMENT;
  std::memset(out_info, 0, sizeof(*out_info));
  if (atlas == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return Impl(atlas)->GetPage(page, out_info);
}

wt_status wt_atlas_set_target(wt_atlas* atlas, uint32_t page,
                              const wt_atlas_target* target, uint32_t height) {
  if (atlas == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return Impl(atlas)->SetTarget(page, target, height);
}

int32_t wt_atlas_shown_page(wt_atlas* atlas, const wt_player* player) {
  if (atlas == nullptr || player == nullptr) return -1;
  return Impl(atlas)->ShownPage(reinterpret_cast<const wt::Player*>(player));
}

}  // extern "C"
