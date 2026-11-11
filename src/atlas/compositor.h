// wt::Atlas: the atlas compositor behind wt_atlas (T4.2, ADR 0020).
//
// An atlas is a set of pages on one Scheduler. Each page has a SlotAllocator
// (ADR 0019), one scheduler Group, and the platform buffer queue it presents
// into (a wt_atlas_target, set by the glue). A player attached to the atlas
// is retargeted (Scheduler::Retarget) to a sink that writes each frame into
// a CPU copy of its slot. When a round's frames for the page are all done,
// the scheduler calls Page::Present on a worker, which locks one buffer of
// the target for the whole page, writes every live slot that fits in it
// (content and cleared gutter, the slot's whole outer rect) and shows it.
//
// Why a CPU copy per slot: a page's buffers come back from the queue with
// stale contents, so every present rewrites every live slot, including the
// ones whose player drew nothing this round (paused, frozen by
// max_concurrent, or between frames). Writing frames straight into the
// locked buffer would mean holding it across a round and reading back from
// graphics memory. A frame is decoded into a spare buffer and swapped in on
// release, so a present never sees a half-written frame.
//
// Threads and locks (outermost first; nothing waits while holding an inner
// one):
// - mu_: attach, detach, set_target, destroy and Leave. Held across
//   Scheduler::Retarget, which waits for a frame in flight; no worker path
//   takes it.
// - Page::target_mu_: the target. Held by Present across acquire, writes and
//   present, and by SetTarget to swap it (so it waits for a present in
//   flight).
// - Page::slots_mu_: the allocator, slots, CPU frames and page size. Held
//   briefly by the slot sink on workers, by Present while it writes the
//   slots, and by attach/detach/get_page.
// - Page::stats_mu_: counters (a leaf).
// The scheduler's own lock is never held while calling into here, and this
// code never calls the scheduler while holding a page lock.
#ifndef WT_ATLAS_COMPOSITOR_H_
#define WT_ATLAS_COMPOSITOR_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "api/wt_api.h"
#include "atlas/slot_allocator.h"
#include "sched/scheduler.h"

namespace wt {

// Rows a page keeps when it could shrink, before it asks for a smaller
// target: max(kShrinkMinRows, height / 4) (see wt_atlas_page_info).
constexpr uint32_t kShrinkMinRows = 128;

// Test hook: what a present wrote, reported while the page's slot lock is
// still held so the view is exact.
struct AtlasSlotView {
  const Player* player = nullptr;
  AtlasRect outer;
  AtlasRect inner;
  // Fits in the target and was written (else it lies below the target's
  // height and the target's texture doesn't show it).
  bool written = false;
  // Content came from a frame; else the whole outer rect was cleared.
  bool has_frame = false;
  uint32_t frame = 0;  // wt_frame_info.index of that frame.
};

class AtlasPresentObserver {
 public:
  virtual ~AtlasPresentObserver() = default;
  // Runs on the presenting worker after the slots were written and before
  // the target's present. `start` is the scheduler's start time of this
  // present (PresentTarget::Present). Must not call into the atlas or the
  // scheduler.
  virtual void OnPresent(uint32_t page, TimePoint start,
                         const wt_buffer& buffer, uint32_t width,
                         uint32_t height,
                         const std::vector<AtlasSlotView>& slots) = 0;
};

class Atlas {
 public:
  static void DefaultOptions(wt_atlas_options* out);
  static bool OptionsValid(const wt_atlas_options& options);

  // `sched` must outlive the atlas.
  static wt_status Create(Scheduler* sched, const wt_atlas_options& options,
                          std::unique_ptr<Atlas>* out);

  // As wt_atlas_destroy.
  ~Atlas();

  Atlas(const Atlas&) = delete;
  Atlas& operator=(const Atlas&) = delete;

  // As wt_atlas_attach, wt_atlas_detach, wt_atlas_page_count,
  // wt_atlas_get_page and wt_atlas_set_target.
  wt_status Attach(Player* player, wt_atlas_slot* out);
  wt_status Detach(Player* player);
  uint32_t page_count() const;
  wt_status GetPage(uint32_t page, wt_atlas_page_info* out) const;
  wt_status SetTarget(uint32_t page, const wt_atlas_target* target,
                      uint32_t height);

  Scheduler* scheduler() const { return sched_; }
  const wt_atlas_options& options() const { return options_; }

  // Tests only: called by every present from now on (nullptr: none). Set it
  // before the first attach.
  void SetObserverForTesting(AtlasPresentObserver* observer);

 private:
  class Page;
  struct SlotEntry;

  // A frame-sized CPU buffer.
  struct Frame {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0;
  };

  Atlas(Scheduler* sched, const wt_atlas_options& options);

  // The slot sink (wt_sink), on scheduler workers.
  static wt_status SlotAcquire(void* context, wt_buffer* out);
  static void SlotRelease(void* context, wt_status status,
                          const wt_frame_info* frame);
  static void SlotError(void* context, wt_status status);

  // Requires mu_. Allocates a slot for width x height on a page, creating
  // the page if needed.
  wt_status Place(uint32_t width, uint32_t height, Page** page, Slot* slot);
  // Requires mu_. Takes the entry out of its page and frees it. The player
  // must no longer draw into it.
  void Drop(SlotEntry* entry);
  // Page::Leave: the player is being destroyed.
  void OnLeave(Player* player);

  Scheduler* const sched_;
  const wt_atlas_options options_;
  std::atomic<AtlasPresentObserver*> observer_{nullptr};

  // Guards players_, page creation and every call that changes slots or
  // targets.
  std::mutex mu_;
  std::unordered_map<const Player*, SlotEntry*> players_;

  // Pages are appended under mu_ and never move or go away before the
  // atlas, so readers find them through page_count_ without mu_.
  std::array<std::unique_ptr<Page>, WT_ATLAS_MAX_PAGES> pages_;
  std::atomic<uint32_t> page_count_{0};
};

}  // namespace wt

#endif  // WT_ATLAS_COMPOSITOR_H_
