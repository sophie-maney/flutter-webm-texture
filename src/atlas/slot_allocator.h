// wt::SlotAllocator: where each animation's frame goes in an atlas page
// (T4.1, ADR 0019). Pure bookkeeping: no pixels, no threads, no platform
// code. The compositor (T4.2) owns one per page, writes frames into the
// rects it hands out, and resizes the page's buffer when page_size() changes.
//
// Layout (ADR 0006, spike C):
// - A page is `width` px wide (at most 2048) and exactly as tall as the
//   shelves in use; there is no fixed 2048-px height. An empty allocator has
//   height 0.
// - A slot has a content rect (the sticker's bucket size) inside an outer
//   rect that adds a transparent gutter of at least 2 px on every side. The
//   outer rect is the slot's pitch: bucket + 2 * gutter, rounded up to a
//   multiple of 16, so every outer rect starts on a 16-px boundary in both
//   axes (128-px bucket -> 144-px pitch, a 8-px gutter right and bottom, 2 px
//   left and top). The compositor must clear the whole outer rect, gutter
//   included, on every write.
// - The page is a stack of shelves from y = 0. A shelf is as tall as its
//   slots' pitch, holds slots of one bucket (width x height) only, and fits
//   floor(width / pitch_w) of them side by side. Slot i is at x = i * pitch_w.
//
// Policy (deterministic; nothing depends on time or addresses):
// - Alloc takes the lowest free column of the lowest shelf that already holds
//   this bucket. Else it reuses an empty shelf (best fit: the shortest one
//   that is tall enough, lowest on a tie; a taller one is split and the rest
//   stays empty). Else it starts a new shelf below the last one, which grows
//   the page. Else it fails with WT_ERR_LIMIT: the page is at max_height.
// - Free marks the column free. A shelf whose slots are all gone becomes
//   empty and merges with empty neighbours; when it reaches the bottom of the
//   page it is dropped and the page shrinks. Slots never move, so a slot's
//   rects are valid until it is freed; the page can still hold rows that a
//   repack would save, which stats().compactable_rows reports. Repacking is
//   the caller's decision: it means rewriting every slot's rect in the widgets.
//
// Threads: none. A SlotAllocator has one owner, which synchronizes any use
// from more than one thread itself (the compositor guards it with its own
// lock). Memory is bounded by the page: at most
// (width / 16) * (max_height / 16) <= 16384 slots.
#ifndef WT_ATLAS_SLOT_ALLOCATOR_H_
#define WT_ATLAS_SLOT_ALLOCATOR_H_

#include <cstdint>
#include <vector>

#include "api/wt_api.h"

namespace wt {

// A rectangle in page pixels, origin top left.
struct AtlasRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// A handle to an allocated slot. 0 is never valid. A handle stays valid until
// Free (or Clear); using it after that fails with WT_ERR_INVALID_ARGUMENT
// (a freed handle is recognised until its index has been reused 65535 times).
using SlotId = uint32_t;
constexpr SlotId kInvalidSlot = 0;

// An allocated slot.
struct Slot {
  SlotId id = kInvalidSlot;
  // The whole pitch: content plus gutter. The compositor clears this, so the
  // gutter stays transparent. 16-px aligned origin and size.
  AtlasRect outer;
  // Where the bucket-sized frame goes: outer inset by `gutter` on the left
  // and top. At least 2 px of outer remain on every side of it.
  AtlasRect inner;
};

struct AtlasOptions {
  // Page width in px. Rounded down to a multiple of 16 and clamped to
  // [16, 2048].
  uint32_t width = 2048;
  // The page never grows past this height. Same rounding and clamp. Lower it
  // to cap a page's buffer memory.
  uint32_t max_height = 2048;
  // Transparent border on each side of a slot's content, clamped to
  // [2, 64]. Slot pitch is content + 2 * gutter rounded up to 16.
  uint32_t gutter = 2;
};

struct AtlasPageSize {
  uint32_t width = 0;
  // The rows in use, a multiple of 16. 0 when no slot is allocated.
  uint32_t height = 0;
};

struct AtlasStats {
  AtlasPageSize page;
  uint32_t max_height = 0;
  uint32_t live_slots = 0;
  // Unused columns on shelves that hold something: room Alloc can take
  // without growing the page.
  uint32_t free_slots = 0;
  // Shelves with at least one live slot / empty shelves inside the page.
  uint32_t shelves = 0;
  uint32_t empty_shelves = 0;
  // Rows (px) in empty shelves, and the tallest empty shelf. An empty shelf
  // can take a new bucket of at most this pitch height.
  uint32_t empty_rows = 0;
  uint32_t largest_empty_rows = 0;
  // Area of live outer rects, and the rest of the page (page width x page
  // height - live_area): gutters beyond the 2-px minimum are in live_area.
  uint64_t live_area = 0;
  uint64_t unused_area = 0;
  // How many rows (px) a full repack, with the same shelf rules and no empty
  // shelves, would take off the page height. Also a lower bound on what
  // dropping every empty or sparse shelf could save.
  uint32_t compactable_rows = 0;
};

class SlotAllocator {
 public:
  explicit SlotAllocator(const AtlasOptions& options = AtlasOptions());

  SlotAllocator(const SlotAllocator&) = delete;
  SlotAllocator& operator=(const SlotAllocator&) = delete;

  // Allocates a slot for frames of width x height px (a size bucket).
  // Returns:
  // - WT_OK, with *out filled;
  // - WT_ERR_INVALID_ARGUMENT for a NULL out, a zero size, or a bucket whose
  //   pitch is wider than the page or taller than max_height (it can never
  //   fit);
  // - WT_ERR_LIMIT when the page is full: nothing free for this bucket and
  //   no room to add a shelf. The page is unchanged; the caller can add a
  //   page, or free slots and retry. *out is left alone.
  wt_status Alloc(uint32_t width, uint32_t height, Slot* out);

  // Releases a slot. Returns WT_ERR_INVALID_ARGUMENT for kInvalidSlot, an
  // unknown or already freed handle. May shrink page_size().
  wt_status Free(SlotId id);

  // Fills *out with the slot's rects. WT_ERR_INVALID_ARGUMENT as for Free.
  wt_status Get(SlotId id, Slot* out) const;

  // Frees everything. The options stay.
  void Clear();

  // Current page size; the compositor resizes its buffer when it differs
  // from the last one it used.
  AtlasPageSize page_size() const;

  // Slots Alloc could hand out for this bucket without touching the layout
  // of other shelves: the free columns of shelves that hold it.
  uint32_t FreeSlotsFor(uint32_t width, uint32_t height) const;

  AtlasStats stats() const;

  // The options after clamping.
  const AtlasOptions& options() const { return options_; }

 private:
  struct Shelf {
    uint32_t y = 0;
    uint32_t height = 0;  // Always a multiple of 16.
    bool bound = false;   // Holds a bucket; else empty.
    uint32_t bucket_width = 0;
    uint32_t bucket_height = 0;
    uint32_t pitch_width = 0;
    uint32_t live = 0;
    std::vector<uint8_t> taken;  // One flag per column.
  };
  struct Entry {
    bool live = false;
    uint16_t generation = 1;
    Slot slot;
    uint32_t bucket_width = 0;
    uint32_t bucket_height = 0;
  };

  Entry* Find(SlotId id);
  const Entry* Find(SlotId id) const;
  size_t ShelfAt(uint32_t y) const;
  uint32_t used_rows() const;
  void BindShelf(Shelf* shelf, uint32_t width, uint32_t height,
                 uint32_t pitch_width);
  wt_status Place(size_t shelf_index, Slot* out);

  AtlasOptions options_;
  std::vector<Shelf> shelves_;  // Contiguous from y = 0, in order.
  std::vector<Entry> entries_;  // Indexed by handle & 0xffff, minus 1.
  std::vector<uint32_t> free_entries_;
  uint32_t live_slots_ = 0;
};

}  // namespace wt

#endif  // WT_ATLAS_SLOT_ALLOCATOR_H_
