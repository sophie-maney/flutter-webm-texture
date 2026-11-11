// The scheduler part of the C ABI in wt_api.h (wt_sched_*, wt_player_*): a
// thin layer over wt::Scheduler. wt_sched and wt_player are never defined;
// the handles are wt::Scheduler and wt::Player pointers.

#include <cstring>
#include <memory>
#include <new>

#include "api/handles.h"
#include "api/wt_api.h"
#include "sched/frame_source.h"
#include "sched/scheduler.h"

namespace {

wt::Scheduler* Impl(wt_sched* s) { return reinterpret_cast<wt::Scheduler*>(s); }
wt::Player* Impl(wt_player* p) { return reinterpret_cast<wt::Player*>(p); }

// Plays a wt_anim, which it owns.
class AnimSource final : public wt::FrameSource {
 public:
  // The size is read here, before the scheduler can use the animation.
  explicit AnimSource(wt_anim* anim) : anim_(anim) {
    wt_anim_get_info(anim, nullptr, &width_, &height_);
  }
  ~AnimSource() override { wt_anim_destroy(anim_); }

  // Gives the animation back to the caller.
  wt_anim* Release() {
    wt_anim* anim = anim_;
    anim_ = nullptr;
    return anim;
  }

  void GetOutputSize(uint32_t* width, uint32_t* height) const override {
    *width = width_;
    *height = height_;
  }
  uint32_t frame_count() const override {
    return anim_->anim->info().frame_count;
  }
  uint32_t frame_duration_ms(uint32_t index) const override {
    return anim_->anim->frame_duration_ms(index);
  }
  wt_status Seek(uint32_t index) override { return anim_->anim->Seek(index); }
  wt_status Skip() override { return anim_->anim->SkipFrame(nullptr); }
  wt_status Draw(const wt_buffer& dst, wt_frame_info* out_frame) override {
    return wt_anim_next_frame(anim_, dst.pixels, dst.size, dst.stride,
                              dst.format, out_frame);
  }

 private:
  wt_anim* anim_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

}  // namespace

extern "C" {

void wt_sched_options_init_default(wt_sched_options* out) {
  if (out == nullptr) return;
  std::memset(out, 0, sizeof(*out));
  wt::Scheduler::DefaultOptions(out);
}

wt_status wt_sched_create(const wt_sched_options* options,
                          const wt_sched_hooks* hooks, wt_sched** out_sched) {
  if (out_sched == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out_sched = nullptr;
  wt_sched_options resolved;
  if (options != nullptr) {
    resolved = *options;
  } else {
    wt_sched_options_init_default(&resolved);
  }
  wt_sched_hooks resolved_hooks{};
  if (hooks != nullptr) resolved_hooks = *hooks;
  std::unique_ptr<wt::Scheduler> sched;
  const wt_status s = wt::Scheduler::Create(resolved, resolved_hooks, &sched);
  if (s != WT_OK) return s;
  *out_sched = reinterpret_cast<wt_sched*>(sched.release());
  return WT_OK;
}

void wt_sched_destroy(wt_sched* sched) { delete Impl(sched); }

wt_status wt_sched_set_options(wt_sched* sched,
                               const wt_sched_options* options) {
  if (sched == nullptr || options == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return Impl(sched)->SetOptions(*options);
}

wt_status wt_sched_get_stats(wt_sched* sched, wt_sched_stats* out_stats) {
  if (out_stats == nullptr) return WT_ERR_INVALID_ARGUMENT;
  std::memset(out_stats, 0, sizeof(*out_stats));
  if (sched == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out_stats = Impl(sched)->Stats();
  return WT_OK;
}

wt_status wt_player_create(wt_sched* sched, wt_anim* anim, bool loop,
                           const wt_sink* sink, wt_player** out_player) {
  if (out_player == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out_player = nullptr;
  if (sched == nullptr || anim == nullptr || sink == nullptr ||
      sink->acquire == nullptr || sink->release == nullptr) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  std::unique_ptr<AnimSource> owned(new (std::nothrow) AnimSource(anim));
  if (owned == nullptr) return WT_ERR_NO_MEMORY;
  AnimSource* raw = owned.get();
  std::unique_ptr<wt::FrameSource> source(std::move(owned));
  wt::Player* player = nullptr;
  const wt_status s = Impl(sched)->AddPlayer(&source, loop, *sink, &player);
  if (s != WT_OK) {
    // On failure the source is still ours: hand the animation back.
    raw->Release();
    return s;
  }
  *out_player = reinterpret_cast<wt_player*>(player);
  return WT_OK;
}

void wt_player_destroy(wt_player* player) {
  if (player == nullptr) return;
  wt::Player* p = Impl(player);
  p->scheduler()->RemovePlayer(p);
}

void wt_player_set_active(wt_player* player, bool active) {
  if (player == nullptr) return;
  wt::Player* p = Impl(player);
  p->scheduler()->SetActive(p, active);
}

void wt_player_set_priority(wt_player* player, uint64_t priority) {
  if (player == nullptr) return;
  wt::Player* p = Impl(player);
  p->scheduler()->SetPriority(p, priority);
}

wt_status wt_player_seek(wt_player* player, uint32_t frame_index) {
  if (player == nullptr) return WT_ERR_INVALID_ARGUMENT;
  wt::Player* p = Impl(player);
  return p->scheduler()->Seek(p, frame_index);
}

void wt_player_redraw(wt_player* player) {
  if (player == nullptr) return;
  wt::Player* p = Impl(player);
  p->scheduler()->Redraw(p);
}

}  // extern "C"
