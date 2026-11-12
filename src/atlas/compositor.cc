#include "atlas/compositor.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace wt {

namespace {

// Spare frames a page keeps for decoding into: at most one frame per worker
// is in flight, so more than this would never be used at once.
constexpr size_t kMaxSpares = WT_SCHED_MAX_WORKERS;

// Distinct buffer ids remembered per target. Queues hold at most 7 buffers
// (Android) and pools a handful (iOS), so this only bounds a misbehaving
// target.
constexpr size_t kMaxBufferIds = 32;

bool FormatValid(wt_pixel_format format) {
  return format == WT_PIXEL_FORMAT_RGBA8888 ||
         format == WT_PIXEL_FORMAT_BGRA8888;
}

uint32_t RoundUp16(uint32_t v) { return (v + 15u) & ~15u; }

// wt_atlas_page_info.wanted_height.
uint32_t WantedHeight(uint32_t used, uint32_t height) {
  if (used == 0) return 0;
  if (height == 0 || used > height) return used;
  const uint32_t slack = std::max(kShrinkMinRows, height / 4);
  return height - used >= slack ? used : height;
}

}  // namespace

struct Atlas::SlotEntry {
  Page* page = nullptr;
  Player* player = nullptr;
  // The player's sink before Attach; Detach gives it back. Written by
  // Retarget before the player first draws into the slot.
  wt_sink home{};
  Slot slot;

  // Guarded by page->slots_mu_.
  Frame front;  // The latest complete frame; empty until the first.
  Frame back;   // The frame being drawn; empty unless writing.
  uint32_t frame = 0;
  bool writing = false;
  // The page's present (Page::posted_) that first showed a frame of this
  // slot on the current target; 0 until one is under way.
  uint64_t shown_in = 0;
};

class Atlas::Page final : public PresentTarget {
 public:
  Page(Atlas* atlas, uint32_t index, const AtlasOptions& layout)
      : atlas_(atlas),
        index_(index),
        width_(layout.width),
        format_(atlas->options_.format),
        alloc_(layout) {}

  wt_status Present(TimePoint start) override;
  void Leave(Player* player) override { atlas_->OnLeave(player); }

  // Requires slots_mu_. A spare frame of exactly `size` bytes, or none.
  Frame TakeSpare(size_t size) {
    for (size_t i = 0; i < spares_.size(); ++i) {
      if (spares_[i].size == size) {
        Frame f = std::move(spares_[i]);
        spares_.erase(spares_.begin() + static_cast<std::ptrdiff_t>(i));
        return f;
      }
    }
    return Frame();
  }

  // Requires slots_mu_. Keeps f for later frames; returns what no longer
  // fits, for the caller to free outside the lock.
  Frame PutSpare(Frame f) {
    Frame evicted;
    if (f.data == nullptr) return evicted;
    if (spares_.size() >= kMaxSpares) {
      evicted = std::move(spares_.front());
      spares_.erase(spares_.begin());
    }
    spares_.push_back(std::move(f));
    return evicted;
  }

  Atlas* const atlas_;
  const uint32_t index_;
  const uint32_t width_;
  const wt_pixel_format format_;
  Group* group_ = nullptr;  // Set before the page is published.

  std::mutex target_mu_;
  // Guarded by target_mu_.
  bool has_target_ = false;
  wt_atlas_target target_{};
  uint32_t target_height_ = 0;

  mutable std::mutex slots_mu_;
  // Guarded by slots_mu_.
  SlotAllocator alloc_;
  std::vector<std::unique_ptr<SlotEntry>> slots_;
  std::vector<Frame> spares_;
  uint32_t height_ = 0;               // As last passed to SetTarget.
  uint64_t posted_ = 0;               // Presents shown, numbered from 1.
  bool target_set_ = false;           // Mirrors has_target_ for GetPage.
  std::vector<AtlasSlotView> views_;  // Observer scratch.

  mutable std::mutex stats_mu_;
  // Guarded by stats_mu_.
  uint64_t presents_ = 0;
  uint64_t failures_ = 0;
  std::vector<uint64_t> buffer_ids_;
  uint32_t stride_ = 0;

 private:
  void CountFailure() {
    std::lock_guard<std::mutex> lk(stats_mu_);
    ++failures_;
  }
};

namespace {

// Writes slot e's whole outer rect into buf: its frame inside the content
// rect and zeros (transparent) everywhere else, or zeros only before its
// first frame. The caller checked that the outer rect lies inside a buffer
// of at least stride * (height - 1) + width * 4 bytes.
template <typename Entry>
void WriteSlot(const wt_buffer& buf, const Entry& e) {
  const AtlasRect& o = e.slot.outer;
  const AtlasRect& in = e.slot.inner;
  const size_t outer_bytes = size_t{o.width} * 4;
  const size_t left = size_t{in.x - o.x} * 4;
  const size_t content = size_t{in.width} * 4;
  const size_t right = outer_bytes - left - content;
  const uint32_t top = in.y - o.y;
  const uint8_t* src = e.front.data.get();
  if (src != nullptr && e.front.size < content * in.height) src = nullptr;
  for (uint32_t y = 0; y < o.height; ++y) {
    uint8_t* row = buf.pixels + size_t{o.y + y} * buf.stride + size_t{o.x} * 4;
    if (src == nullptr || y < top || y - top >= in.height) {
      std::memset(row, 0, outer_bytes);
      continue;
    }
    std::memset(row, 0, left);
    std::memcpy(row + left, src + size_t{y - top} * content, content);
    std::memset(row + left + content, 0, right);
  }
}

}  // namespace

wt_status Atlas::Page::Present(TimePoint start) {
  std::lock_guard<std::mutex> tl(target_mu_);
  if (!has_target_) return WT_NO_TARGET;
  wt_buffer buf{};
  uint64_t id = 0;
  const wt_status status = target_.acquire(target_.context, &buf, &id);
  if (status == WT_BUSY || status == WT_NO_TARGET) return status;
  if (status != WT_OK) {
    CountFailure();
    return status < 0 ? status : WT_ERR_INTERNAL;
  }
  const uint32_t height = target_height_;
  const size_t row = size_t{width_} * 4;
  // In 64 bits, like CheckPixelBuffer: a 32-bit size_t (armeabi-v7a) could
  // wrap for a bogus stride and let a too-small buffer through.
  if (buf.pixels == nullptr || buf.format != format_ || buf.stride < row ||
      buf.size < uint64_t{buf.stride} * (height - 1) + row) {
    target_.present(target_.context, WT_ERR_INTERNAL);
    CountFailure();
    return WT_ERR_INTERNAL;
  }
  AtlasPresentObserver* observer =
      atlas_->observer_.load(std::memory_order_acquire);
  uint64_t seq = 0;
  {
    std::lock_guard<std::mutex> sl(slots_mu_);
    seq = posted_ + 1;
    if (observer != nullptr) views_.clear();
    for (const std::unique_ptr<SlotEntry>& e : slots_) {
      const AtlasRect& o = e->slot.outer;
      const bool fits = o.x + o.width <= width_ && o.y + o.height <= height;
      if (fits) {
        WriteSlot(buf, *e);
        if (e->shown_in == 0 && e->front.data != nullptr) e->shown_in = seq;
      }
      if (observer != nullptr) {
        AtlasSlotView v;
        v.player = e->player;
        v.outer = o;
        v.inner = e->slot.inner;
        v.written = fits;
        v.has_frame = e->front.data != nullptr;
        v.frame = e->frame;
        views_.push_back(v);
      }
    }
    if (observer != nullptr) {
      observer->OnPresent(index_, start, buf, width_, height, views_);
    }
  }
  target_.present(target_.context, WT_OK);
  {
    // Slots this present wrote count as shown from here on (ShownPage):
    // the buffer is on its way to the screen.
    std::lock_guard<std::mutex> sl(slots_mu_);
    posted_ = seq;
  }
  const uint64_t key = id != 0 ? id : reinterpret_cast<uintptr_t>(buf.pixels);
  std::lock_guard<std::mutex> lk(stats_mu_);
  ++presents_;
  stride_ = buf.stride;
  if (buffer_ids_.size() < kMaxBufferIds &&
      std::find(buffer_ids_.begin(), buffer_ids_.end(), key) ==
          buffer_ids_.end()) {
    buffer_ids_.push_back(key);
  }
  return WT_OK;
}

// --- Atlas ---------------------------------------------------------------

void Atlas::DefaultOptions(wt_atlas_options* out) {
  out->width = 2048;
  out->max_height = 2048;
  out->gutter = 2;
  out->max_pages = 4;
  out->format = WT_PIXEL_FORMAT_RGBA8888;
}

bool Atlas::OptionsValid(const wt_atlas_options& o) {
  return o.width >= 16 && o.width <= 2048 && o.max_height >= 16 &&
         o.max_height <= 2048 && o.gutter >= 2 && o.gutter <= 64 &&
         o.max_pages >= 1 && o.max_pages <= WT_ATLAS_MAX_PAGES &&
         FormatValid(o.format);
}

Atlas::Atlas(Scheduler* sched, const wt_atlas_options& options)
    : sched_(sched), options_(options) {}

wt_status Atlas::Create(Scheduler* sched, const wt_atlas_options& options,
                        std::unique_ptr<Atlas>* out) {
  out->reset();
  if (sched == nullptr || !OptionsValid(options)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  wt_atlas_options normalized = options;
  normalized.width &= ~15u;
  normalized.max_height &= ~15u;
  out->reset(new (std::nothrow) Atlas(sched, normalized));
  return *out == nullptr ? WT_ERR_NO_MEMORY : WT_OK;
}

Atlas::~Atlas() {
  std::lock_guard<std::mutex> lk(mu_);
  // Every player goes back to its own sink. A player that can't (it is being
  // destroyed right now, which the API forbids) is dropped all the same.
  std::vector<SlotEntry*> entries;
  entries.reserve(players_.size());
  for (const auto& kv : players_) entries.push_back(kv.second);
  for (SlotEntry* e : entries) {
    sched_->Retarget(e->player, e->home, nullptr, e->page->group_, nullptr);
    Drop(e);
  }
  // Waits for a present in flight; none starts afterwards.
  const uint32_t count = page_count_.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < count; ++i) {
    sched_->RemoveGroup(pages_[i]->group_);
  }
}

void Atlas::SetObserverForTesting(AtlasPresentObserver* observer) {
  observer_.store(observer, std::memory_order_release);
}

uint32_t Atlas::page_count() const {
  return page_count_.load(std::memory_order_acquire);
}

wt_status Atlas::Place(uint32_t width, uint32_t height, Page** out_page,
                       Slot* out_slot) {
  const uint32_t g = options_.gutter;
  if (RoundUp16(width + 2 * g) > options_.width ||
      RoundUp16(height + 2 * g) > options_.max_height) {
    return WT_ERR_INVALID_ARGUMENT;  // Would never fit a page.
  }
  const uint32_t count = page_count_.load(std::memory_order_relaxed);
  // A free slot of this size first: no page grows.
  for (uint32_t i = 0; i < count; ++i) {
    Page* page = pages_[i].get();
    std::lock_guard<std::mutex> lk(page->slots_mu_);
    if (page->alloc_.FreeSlotsFor(width, height) > 0 &&
        page->alloc_.Alloc(width, height, out_slot) == WT_OK) {
      *out_page = page;
      return WT_OK;
    }
  }
  // Then any page that can reuse or add rows for it.
  for (uint32_t i = 0; i < count; ++i) {
    Page* page = pages_[i].get();
    std::lock_guard<std::mutex> lk(page->slots_mu_);
    const wt_status s = page->alloc_.Alloc(width, height, out_slot);
    if (s == WT_OK) {
      *out_page = page;
      return WT_OK;
    }
    if (s != WT_ERR_LIMIT) return s;
  }
  // Then a new page.
  if (count >= options_.max_pages) return WT_ERR_LIMIT;
  AtlasOptions layout;
  layout.width = options_.width;
  layout.max_height = options_.max_height;
  layout.gutter = options_.gutter;
  std::unique_ptr<Page> page(new (std::nothrow) Page(this, count, layout));
  if (page == nullptr) return WT_ERR_NO_MEMORY;
  wt_status s = page->alloc_.Alloc(width, height, out_slot);
  if (s != WT_OK) return s;
  s = sched_->AddGroup(page.get(), &page->group_);
  if (s != WT_OK) return s;
  *out_page = page.get();
  pages_[count] = std::move(page);
  page_count_.store(count + 1, std::memory_order_release);
  return WT_OK;
}

wt_status Atlas::Attach(Player* player, wt_atlas_slot* out) {
  if (player == nullptr || out == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out = wt_atlas_slot{};
  if (player->scheduler() != sched_) return WT_ERR_INVALID_ARGUMENT;
  uint32_t width = 0;
  uint32_t height = 0;
  player->source().GetOutputSize(&width, &height);
  if (width == 0 || height == 0) return WT_ERR_INVALID_ARGUMENT;

  std::lock_guard<std::mutex> lk(mu_);
  if (players_.count(player) != 0) return WT_ERR_INVALID_ARGUMENT;
  Page* page = nullptr;
  Slot slot;
  wt_status s = Place(width, height, &page, &slot);
  if (s != WT_OK) return s;
  std::unique_ptr<SlotEntry> owned(new (std::nothrow) SlotEntry);
  if (owned == nullptr) {
    std::lock_guard<std::mutex> sl(page->slots_mu_);
    page->alloc_.Free(slot.id);
    return WT_ERR_NO_MEMORY;
  }
  SlotEntry* e = owned.get();
  e->page = page;
  e->player = player;
  e->slot = slot;
  {
    std::lock_guard<std::mutex> sl(page->slots_mu_);
    page->slots_.push_back(std::move(owned));
  }
  players_[player] = e;

  wt_sink sink{};
  sink.context = e;
  sink.acquire = &Atlas::SlotAcquire;
  sink.release = &Atlas::SlotRelease;
  sink.error = &Atlas::SlotError;
  // Waits for a frame in flight on the old sink, then asks for the current
  // frame, which lands in the slot.
  s = sched_->Retarget(player, sink, page->group_, nullptr, &e->home);
  if (s != WT_OK) {
    Drop(e);  // In another atlas, or being destroyed.
    return s;
  }
  out->page = page->index_;
  out->x = slot.inner.x;
  out->y = slot.inner.y;
  out->width = slot.inner.width;
  out->height = slot.inner.height;
  out->outer_x = slot.outer.x;
  out->outer_y = slot.outer.y;
  out->outer_width = slot.outer.width;
  out->outer_height = slot.outer.height;
  return WT_OK;
}

wt_status Atlas::Detach(Player* player) {
  if (player == nullptr) return WT_ERR_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = players_.find(player);
  if (it == players_.end()) return WT_ERR_INVALID_ARGUMENT;
  SlotEntry* e = it->second;
  // Waits for a frame in flight into the slot; the home sink gets the
  // current frame next.
  const wt_status s =
      sched_->Retarget(player, e->home, nullptr, e->page->group_, nullptr);
  if (s != WT_OK) return s;
  Drop(e);
  return WT_OK;
}

void Atlas::OnLeave(Player* player) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = players_.find(player);
  if (it != players_.end()) Drop(it->second);
}

void Atlas::Drop(SlotEntry* e) {
  players_.erase(e->player);
  Page* page = e->page;
  std::unique_ptr<SlotEntry> owned;
  std::vector<Frame> freed;
  {
    std::lock_guard<std::mutex> sl(page->slots_mu_);
    page->alloc_.Free(e->slot.id);
    for (size_t i = 0; i < page->slots_.size(); ++i) {
      if (page->slots_[i].get() == e) {
        owned = std::move(page->slots_[i]);
        page->slots_.erase(page->slots_.begin() +
                           static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    // An empty page keeps no CPU memory.
    if (page->slots_.empty()) freed.swap(page->spares_);
  }
  // Frames are freed here, without the lock.
}

wt_status Atlas::GetPage(uint32_t index, wt_atlas_page_info* out) const {
  if (out == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out = wt_atlas_page_info{};
  if (index >= page_count_.load(std::memory_order_acquire)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  const Page* page = pages_[index].get();
  out->width = page->width_;
  {
    std::lock_guard<std::mutex> sl(page->slots_mu_);
    out->used_height = page->alloc_.page_size().height;
    out->height = page->height_;
    out->has_target = page->target_set_ ? 1 : 0;
    out->live_slots = static_cast<uint32_t>(page->slots_.size());
    uint64_t cpu = 0;
    for (const std::unique_ptr<SlotEntry>& e : page->slots_) {
      cpu += e->front.size + e->back.size;
    }
    for (const Frame& f : page->spares_) cpu += f.size;
    out->cpu_bytes = cpu;
  }
  out->wanted_height = WantedHeight(out->used_height, out->height);
  std::lock_guard<std::mutex> lk(page->stats_mu_);
  out->buffers = static_cast<uint32_t>(page->buffer_ids_.size());
  out->buffer_stride = page->stride_;
  out->buffer_bytes = uint64_t{out->buffers} * page->stride_ *
                      (out->has_target ? out->height : 0u);
  out->presents = page->presents_;
  out->present_failures = page->failures_;
  return WT_OK;
}

wt_status Atlas::SetTarget(uint32_t index, const wt_atlas_target* target,
                           uint32_t height) {
  if (index >= page_count_.load(std::memory_order_acquire)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  if (height % 16 != 0 || height > options_.max_height) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  if (target != nullptr && (height == 0 || target->acquire == nullptr ||
                            target->present == nullptr)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lk(mu_);
  Page* page = pages_[index].get();
  {
    // Waits for a present in flight on the old target.
    std::lock_guard<std::mutex> tl(page->target_mu_);
    page->has_target_ = target != nullptr;
    page->target_ = target != nullptr ? *target : wt_atlas_target{};
    page->target_height_ = target != nullptr ? height : 0;
    std::lock_guard<std::mutex> sl(page->slots_mu_);
    page->height_ = height;
    page->target_set_ = target != nullptr;
    // The new target (or none) shows no slot yet.
    for (const std::unique_ptr<SlotEntry>& e : page->slots_) e->shown_in = 0;
    std::lock_guard<std::mutex> st(page->stats_mu_);
    page->buffer_ids_.clear();
    page->stride_ = 0;
  }
  if (target != nullptr) sched_->RequestPresent(page->group_);
  return WT_OK;
}

int32_t Atlas::ShownPage(const Player* player) {
  std::lock_guard<std::mutex> lk(mu_);
  const auto it = players_.find(player);
  if (it == players_.end()) return -1;
  const SlotEntry* e = it->second;
  const Page* page = e->page;
  std::lock_guard<std::mutex> sl(page->slots_mu_);
  const bool shown = e->shown_in != 0 && e->shown_in <= page->posted_;
  return shown ? static_cast<int32_t>(page->index_) : -1;
}

// --- Slot sink (scheduler workers, one call at a time per slot) -----------

wt_status Atlas::SlotAcquire(void* context, wt_buffer* out) {
  auto* e = static_cast<SlotEntry*>(context);
  Page* page = e->page;
  const uint32_t width = e->slot.inner.width;
  const uint32_t height = e->slot.inner.height;
  const size_t size = size_t{width} * height * 4;
  Frame frame;
  {
    std::lock_guard<std::mutex> sl(page->slots_mu_);
    if (e->writing) return WT_ERR_INTERNAL;  // One frame in flight, always.
    frame = page->TakeSpare(size);
  }
  if (frame.data == nullptr) {
    frame.data.reset(new (std::nothrow) uint8_t[size]);
    if (frame.data == nullptr) return WT_ERR_NO_MEMORY;
    frame.size = size;
  }
  out->pixels = frame.data.get();
  out->size = size;
  out->stride = width * 4;
  out->format = page->format_;
  std::lock_guard<std::mutex> sl(page->slots_mu_);
  e->back = std::move(frame);
  e->writing = true;
  return WT_OK;
}

void Atlas::SlotRelease(void* context, wt_status status,
                        const wt_frame_info* frame) {
  auto* e = static_cast<SlotEntry*>(context);
  Page* page = e->page;
  Frame evicted;
  std::lock_guard<std::mutex> sl(page->slots_mu_);
  e->writing = false;
  if (status == WT_OK) {
    // The next present shows it; a present running now still has the
    // previous frame, whole.
    std::swap(e->front, e->back);
    e->frame = frame != nullptr ? frame->index : 0;
  }
  evicted = page->PutSpare(std::move(e->back));
  e->back = Frame();
}

void Atlas::SlotError(void* context, wt_status status) {
  auto* e = static_cast<SlotEntry*>(context);
  // The glue reports decode failures through the player's own sink.
  if (e->home.error != nullptr) e->home.error(e->home.context, status);
}

}  // namespace wt
