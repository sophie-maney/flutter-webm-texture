#include "atlas/slot_allocator.h"

#include <algorithm>
#include <map>

namespace wt {
namespace {

constexpr uint32_t kAlign = 16;
constexpr uint32_t kMaxPageSide = 2048;
constexpr uint32_t kMinGutter = 2;
constexpr uint32_t kMaxGutter = 64;

uint32_t RoundUp(uint32_t v) { return (v + kAlign - 1) / kAlign * kAlign; }

uint32_t ClampSide(uint32_t v) {
  return std::min(std::max(v / kAlign * kAlign, kAlign), kMaxPageSide);
}

uint64_t BucketKey(uint32_t w, uint32_t h) {
  return (static_cast<uint64_t>(w) << 32) | h;
}

// Handles: low 16 bits are the entry index + 1 (never 0), high 16 the
// entry's generation.
SlotId MakeId(uint32_t index, uint16_t generation) {
  return (static_cast<uint32_t>(generation) << 16) | (index + 1);
}

}  // namespace

SlotAllocator::SlotAllocator(const AtlasOptions& options) {
  options_.width = ClampSide(options.width);
  options_.max_height = ClampSide(options.max_height);
  options_.gutter = std::min(std::max(options.gutter, kMinGutter), kMaxGutter);
}

uint32_t SlotAllocator::used_rows() const {
  return shelves_.empty() ? 0 : shelves_.back().y + shelves_.back().height;
}

AtlasPageSize SlotAllocator::page_size() const {
  AtlasPageSize size;
  size.width = options_.width;
  size.height = RoundUp(used_rows());
  return size;
}

SlotAllocator::Entry* SlotAllocator::Find(SlotId id) {
  return const_cast<Entry*>(static_cast<const SlotAllocator*>(this)->Find(id));
}

const SlotAllocator::Entry* SlotAllocator::Find(SlotId id) const {
  const uint32_t low = id & 0xffffu;
  if (low == 0 || low > entries_.size()) return nullptr;
  const Entry& e = entries_[low - 1];
  if (!e.live || e.generation != static_cast<uint16_t>(id >> 16)) {
    return nullptr;
  }
  return &e;
}

size_t SlotAllocator::ShelfAt(uint32_t y) const {
  auto it = std::lower_bound(
      shelves_.begin(), shelves_.end(), y,
      [](const Shelf& s, uint32_t value) { return s.y < value; });
  if (it == shelves_.end() || it->y != y) return shelves_.size();
  return static_cast<size_t>(it - shelves_.begin());
}

void SlotAllocator::BindShelf(Shelf* shelf, uint32_t width, uint32_t height,
                              uint32_t pitch_width) {
  shelf->bound = true;
  shelf->bucket_width = width;
  shelf->bucket_height = height;
  shelf->pitch_width = pitch_width;
  shelf->live = 0;
  shelf->taken.assign(options_.width / pitch_width, 0);
}

wt_status SlotAllocator::Place(size_t shelf_index, Slot* out) {
  Shelf& shelf = shelves_[shelf_index];
  size_t col = 0;
  while (col < shelf.taken.size() && shelf.taken[col]) ++col;
  if (col == shelf.taken.size()) return WT_ERR_INTERNAL;

  uint32_t index;
  if (!free_entries_.empty()) {
    index = free_entries_.back();
    free_entries_.pop_back();
  } else {
    index = static_cast<uint32_t>(entries_.size());
    entries_.emplace_back();
  }
  Entry& e = entries_[index];
  e.live = true;
  e.bucket_width = shelf.bucket_width;
  e.bucket_height = shelf.bucket_height;
  e.slot.id = MakeId(index, e.generation);
  e.slot.outer = {static_cast<uint32_t>(col) * shelf.pitch_width, shelf.y,
                  shelf.pitch_width, shelf.height};
  e.slot.inner = {e.slot.outer.x + options_.gutter,
                  e.slot.outer.y + options_.gutter, shelf.bucket_width,
                  shelf.bucket_height};
  shelf.taken[col] = 1;
  ++shelf.live;
  ++live_slots_;
  *out = e.slot;
  return WT_OK;
}

wt_status SlotAllocator::Alloc(uint32_t width, uint32_t height, Slot* out) {
  if (out == nullptr || width == 0 || height == 0) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  // Written so the sums can't overflow for any uint32 input.
  if (width > options_.width || height > options_.max_height) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  const uint32_t pitch_w = RoundUp(width + 2 * options_.gutter);
  const uint32_t pitch_h = RoundUp(height + 2 * options_.gutter);
  if (pitch_w > options_.width || pitch_h > options_.max_height) {
    return WT_ERR_INVALID_ARGUMENT;
  }

  // 1. A shelf that already holds this bucket and has a free column.
  for (size_t i = 0; i < shelves_.size(); ++i) {
    const Shelf& s = shelves_[i];
    if (s.bound && s.bucket_width == width && s.bucket_height == height &&
        s.live < s.taken.size()) {
      return Place(i, out);
    }
  }

  // 2. An empty shelf: the shortest that is tall enough, lowest first.
  size_t best = shelves_.size();
  for (size_t i = 0; i < shelves_.size(); ++i) {
    const Shelf& s = shelves_[i];
    if (!s.bound && s.height >= pitch_h &&
        (best == shelves_.size() || s.height < shelves_[best].height)) {
      best = i;
    }
  }
  if (best != shelves_.size()) {
    if (shelves_[best].height > pitch_h) {
      Shelf rest;
      rest.y = shelves_[best].y + pitch_h;
      rest.height = shelves_[best].height - pitch_h;
      shelves_[best].height = pitch_h;
      shelves_.insert(shelves_.begin() + static_cast<ptrdiff_t>(best) + 1,
                      std::move(rest));
    }
    BindShelf(&shelves_[best], width, height, pitch_w);
    return Place(best, out);
  }

  // 3. A new shelf at the bottom, which grows the page.
  const uint32_t top = used_rows();
  if (pitch_h > options_.max_height - top) return WT_ERR_LIMIT;
  Shelf shelf;
  shelf.y = top;
  shelf.height = pitch_h;
  BindShelf(&shelf, width, height, pitch_w);
  shelves_.push_back(std::move(shelf));
  return Place(shelves_.size() - 1, out);
}

wt_status SlotAllocator::Free(SlotId id) {
  Entry* e = Find(id);
  if (e == nullptr) return WT_ERR_INVALID_ARGUMENT;

  size_t at = ShelfAt(e->slot.outer.y);
  if (at == shelves_.size() || !shelves_[at].bound) return WT_ERR_INTERNAL;
  Shelf& shelf = shelves_[at];
  const size_t col = e->slot.outer.x / shelf.pitch_width;
  if (col >= shelf.taken.size() || !shelf.taken[col]) return WT_ERR_INTERNAL;
  shelf.taken[col] = 0;
  --shelf.live;
  --live_slots_;

  const uint32_t index = (id & 0xffffu) - 1;
  e->live = false;
  e->generation =
      static_cast<uint16_t>(e->generation == 0xffffu ? 1 : e->generation + 1);
  free_entries_.push_back(index);

  if (shelf.live == 0) {
    shelf.bound = false;
    shelf.taken.clear();
    shelf.bucket_width = shelf.bucket_height = shelf.pitch_width = 0;
    if (at + 1 < shelves_.size() && !shelves_[at + 1].bound) {
      shelves_[at].height += shelves_[at + 1].height;
      shelves_.erase(shelves_.begin() + static_cast<ptrdiff_t>(at) + 1);
    }
    if (at > 0 && !shelves_[at - 1].bound) {
      shelves_[at - 1].height += shelves_[at].height;
      shelves_.erase(shelves_.begin() + static_cast<ptrdiff_t>(at));
      --at;
    }
    if (at + 1 == shelves_.size()) shelves_.pop_back();
  }
  return WT_OK;
}

wt_status SlotAllocator::Get(SlotId id, Slot* out) const {
  const Entry* e = Find(id);
  if (e == nullptr || out == nullptr) return WT_ERR_INVALID_ARGUMENT;
  *out = e->slot;
  return WT_OK;
}

void SlotAllocator::Clear() {
  shelves_.clear();
  free_entries_.clear();
  // Keep generations so handles from before the Clear stay invalid.
  for (size_t i = 0; i < entries_.size(); ++i) {
    Entry& e = entries_[i];
    if (e.live) {
      e.live = false;
      e.generation =
          static_cast<uint16_t>(e.generation == 0xffffu ? 1 : e.generation + 1);
    }
    free_entries_.push_back(static_cast<uint32_t>(entries_.size() - 1 - i));
  }
  live_slots_ = 0;
}

uint32_t SlotAllocator::FreeSlotsFor(uint32_t width, uint32_t height) const {
  uint32_t n = 0;
  for (const Shelf& s : shelves_) {
    if (s.bound && s.bucket_width == width && s.bucket_height == height) {
      n += static_cast<uint32_t>(s.taken.size()) - s.live;
    }
  }
  return n;
}

AtlasStats SlotAllocator::stats() const {
  AtlasStats st;
  st.page = page_size();
  st.max_height = options_.max_height;
  st.live_slots = live_slots_;

  struct Group {
    uint32_t live = 0;
    uint32_t columns = 0;
    uint32_t pitch_height = 0;
  };
  std::map<uint64_t, Group> groups;
  for (const Shelf& s : shelves_) {
    if (s.bound) {
      ++st.shelves;
      st.free_slots += static_cast<uint32_t>(s.taken.size()) - s.live;
      st.live_area += static_cast<uint64_t>(s.live) * s.pitch_width * s.height;
      Group& g = groups[BucketKey(s.bucket_width, s.bucket_height)];
      g.live += s.live;
      g.columns = static_cast<uint32_t>(s.taken.size());
      g.pitch_height = s.height;
    } else {
      ++st.empty_shelves;
      st.empty_rows += s.height;
      st.largest_empty_rows = std::max(st.largest_empty_rows, s.height);
    }
  }
  st.unused_area =
      static_cast<uint64_t>(st.page.width) * st.page.height - st.live_area;

  uint32_t dense = 0;
  for (const auto& kv : groups) {
    const Group& g = kv.second;
    dense += (g.live + g.columns - 1) / g.columns * g.pitch_height;
  }
  st.compactable_rows = st.page.height - dense;
  return st;
}

}  // namespace wt
