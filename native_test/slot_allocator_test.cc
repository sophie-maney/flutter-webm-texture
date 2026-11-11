// Tests for the atlas slot allocator (T4.1, ADR 0019): the layout rules
// (shelves per bucket, 16-px alignment, gutters, page height), the free and
// reuse policy, errors, and a seeded randomized alloc/free test that checks
// every invariant after every operation against a model kept by the test.
#include "atlas/slot_allocator.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "wt_api.h"

namespace {

using wt::AtlasOptions;
using wt::AtlasRect;
using wt::AtlasStats;
using wt::Slot;
using wt::SlotAllocator;
using wt::SlotId;

uint32_t RoundUp16(uint32_t v) { return (v + 15) / 16 * 16; }

Slot MustAlloc(SlotAllocator* a, uint32_t w, uint32_t h) {
  Slot s;
  EXPECT_EQ(a->Alloc(w, h, &s), WT_OK) << w << "x" << h;
  return s;
}

bool Overlap(const AtlasRect& a, const AtlasRect& b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
         b.y < a.y + a.height;
}

bool Same(const AtlasRect& a, const AtlasRect& b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

TEST(SlotAllocatorTest, EmptyPageHasNoHeight) {
  SlotAllocator a;
  EXPECT_EQ(a.page_size().width, 2048u);
  EXPECT_EQ(a.page_size().height, 0u);
  const AtlasStats st = a.stats();
  EXPECT_EQ(st.live_slots, 0u);
  EXPECT_EQ(st.free_slots, 0u);
  EXPECT_EQ(st.shelves, 0u);
  EXPECT_EQ(st.empty_rows, 0u);
  EXPECT_EQ(st.compactable_rows, 0u);
  EXPECT_EQ(st.live_area, 0u);
}

TEST(SlotAllocatorTest, FirstSlotLayout) {
  SlotAllocator a;
  const Slot s = MustAlloc(&a, 128, 128);
  EXPECT_NE(s.id, wt::kInvalidSlot);
  // 128 + 2 * 2 = 132, rounded up to 144.
  EXPECT_TRUE(Same(s.outer, {0, 0, 144, 144}));
  EXPECT_TRUE(Same(s.inner, {2, 2, 128, 128}));
  EXPECT_EQ(a.page_size().height, 144u);
  Slot got;
  ASSERT_EQ(a.Get(s.id, &got), WT_OK);
  EXPECT_EQ(got.id, s.id);
  EXPECT_TRUE(Same(got.outer, s.outer));
  EXPECT_TRUE(Same(got.inner, s.inner));
}

TEST(SlotAllocatorTest, NonAlignedBucketsRoundThePitchUp) {
  SlotAllocator a;
  const Slot s = MustAlloc(&a, 100, 60);
  EXPECT_TRUE(Same(s.outer, {0, 0, 112, 64}));
  EXPECT_TRUE(Same(s.inner, {2, 2, 100, 60}));
  // 124 + 4 would be 128 exactly: no extra rounding.
  const Slot t = MustAlloc(&a, 124, 124);
  EXPECT_EQ(t.outer.width, 128u);
  EXPECT_EQ(t.outer.height, 128u);
  // 125 + 4 = 129 pushes it to the next multiple.
  const Slot u = MustAlloc(&a, 125, 1);
  EXPECT_EQ(u.outer.width, 144u);
  EXPECT_EQ(u.outer.height, 16u);
}

TEST(SlotAllocatorTest, ShelfFillsLeftToRightThenOpensANewOne) {
  SlotAllocator a;
  // 2048 / 144 = 14 columns.
  std::vector<Slot> slots;
  for (int i = 0; i < 14; ++i) slots.push_back(MustAlloc(&a, 128, 128));
  for (int i = 0; i < 14; ++i) {
    EXPECT_EQ(slots[static_cast<size_t>(i)].outer.x,
              static_cast<uint32_t>(i) * 144);
    EXPECT_EQ(slots[static_cast<size_t>(i)].outer.y, 0u);
  }
  EXPECT_EQ(a.page_size().height, 144u);
  EXPECT_EQ(a.stats().free_slots, 0u);
  const Slot next = MustAlloc(&a, 128, 128);
  EXPECT_EQ(next.outer.x, 0u);
  EXPECT_EQ(next.outer.y, 144u);
  EXPECT_EQ(a.page_size().height, 288u);
  EXPECT_EQ(a.stats().free_slots, 13u);
  EXPECT_EQ(a.FreeSlotsFor(128, 128), 13u);
  EXPECT_EQ(a.FreeSlotsFor(64, 64), 0u);
}

TEST(SlotAllocatorTest, ABucketOwnsItsShelf) {
  SlotAllocator a;
  const Slot big = MustAlloc(&a, 128, 128);
  const Slot small = MustAlloc(&a, 64, 64);
  const Slot big2 = MustAlloc(&a, 128, 128);
  // 64 + 4 -> 80; a different bucket never shares a row of the 128 shelf.
  EXPECT_EQ(small.outer.y, 144u);
  EXPECT_EQ(small.outer.height, 80u);
  EXPECT_EQ(big2.outer.y, big.outer.y);
  EXPECT_EQ(big2.outer.x, 144u);
  // Same pitch but another bucket still gets its own shelf.
  const Slot wide = MustAlloc(&a, 128, 124);
  EXPECT_NE(wide.outer.y, big.outer.y);
  EXPECT_EQ(a.page_size().height, 144u + 80u + 128u);  // 124 + 4 = 128.
}

TEST(SlotAllocatorTest, FreeingAnEmptyBottomShelfShrinksThePage) {
  SlotAllocator a;
  const Slot s1 = MustAlloc(&a, 128, 128);
  const Slot s2 = MustAlloc(&a, 64, 64);
  EXPECT_EQ(a.page_size().height, 224u);
  ASSERT_EQ(a.Free(s2.id), WT_OK);
  EXPECT_EQ(a.page_size().height, 144u);
  ASSERT_EQ(a.Free(s1.id), WT_OK);
  EXPECT_EQ(a.page_size().height, 0u);
  EXPECT_EQ(a.stats().live_slots, 0u);
}

TEST(SlotAllocatorTest, ShrinkCascadesThroughEmptyShelvesAboveTheBottom) {
  SlotAllocator a;
  const Slot s1 = MustAlloc(&a, 128, 128);  // y 0
  const Slot s2 = MustAlloc(&a, 64, 64);    // y 144
  const Slot s3 = MustAlloc(&a, 96, 96);    // y 224
  ASSERT_EQ(a.Free(s2.id), WT_OK);          // Middle: stays as empty rows.
  EXPECT_EQ(a.page_size().height, 224u + s3.outer.height);
  EXPECT_EQ(a.stats().empty_rows, 80u);
  ASSERT_EQ(a.Free(s3.id), WT_OK);  // Bottom goes, and the gap with it.
  EXPECT_EQ(a.page_size().height, 144u);
  EXPECT_EQ(a.stats().empty_shelves, 0u);
  ASSERT_EQ(a.Free(s1.id), WT_OK);
  EXPECT_EQ(a.page_size().height, 0u);
}

TEST(SlotAllocatorTest, EmptyShelvesAreReusedAndSplit) {
  SlotAllocator a;
  const Slot big = MustAlloc(&a, 256, 256);  // 272 rows, y 0
  const Slot keep = MustAlloc(&a, 64, 64);   // y 272
  ASSERT_EQ(a.Free(big.id), WT_OK);
  EXPECT_EQ(a.stats().empty_shelves, 1u);
  EXPECT_EQ(a.stats().empty_rows, 272u);
  EXPECT_EQ(a.stats().compactable_rows, 272u);
  EXPECT_EQ(a.page_size().height, 272u + 80u);

  // A smaller bucket takes the top of the empty shelf; the rest stays empty.
  const Slot s = MustAlloc(&a, 128, 128);
  EXPECT_EQ(s.outer.y, 0u);
  EXPECT_EQ(a.page_size().height, 272u + 80u);
  EXPECT_EQ(a.stats().empty_shelves, 1u);
  EXPECT_EQ(a.stats().empty_rows, 272u - 144u);
  EXPECT_EQ(a.stats().largest_empty_rows, 128u);

  // Too tall for the leftover rows: goes below, growing the page.
  const Slot tall = MustAlloc(&a, 160, 160);  // 176 > 128
  EXPECT_EQ(tall.outer.y, 352u);
  EXPECT_EQ(a.page_size().height, 352u + 176u);

  // Empty neighbours merge: free `s`, and the shelf is 272 tall again.
  ASSERT_EQ(a.Free(s.id), WT_OK);
  EXPECT_EQ(a.stats().empty_shelves, 1u);
  EXPECT_EQ(a.stats().largest_empty_rows, 272u);
  (void)keep;
}

TEST(SlotAllocatorTest, BestFitPicksTheShortestEmptyShelf) {
  SlotAllocator a;
  const Slot a1 = MustAlloc(&a, 256, 256);  // 272 at y 0
  const Slot sep1 = MustAlloc(&a, 16, 16);  // 32 at 272
  const Slot a2 = MustAlloc(&a, 128, 128);  // 144 at 304
  const Slot sep2 = MustAlloc(&a, 32, 32);  // 48 at 448
  ASSERT_EQ(a.Free(a1.id), WT_OK);
  ASSERT_EQ(a.Free(a2.id), WT_OK);
  const Slot s = MustAlloc(&a, 100, 100);  // 112 fits both: takes y 304.
  EXPECT_EQ(s.outer.y, 304u);
  (void)sep1;
  (void)sep2;
}

TEST(SlotAllocatorTest, FreedSlotsAreReusedLowestColumnFirst) {
  SlotAllocator a;
  std::vector<Slot> slots;
  for (int i = 0; i < 5; ++i) slots.push_back(MustAlloc(&a, 128, 128));
  ASSERT_EQ(a.Free(slots[1].id), WT_OK);
  ASSERT_EQ(a.Free(slots[3].id), WT_OK);
  const Slot r = MustAlloc(&a, 128, 128);
  EXPECT_EQ(r.outer.x, 144u);
  const Slot r2 = MustAlloc(&a, 128, 128);
  EXPECT_EQ(r2.outer.x, 3u * 144u);
  EXPECT_EQ(a.page_size().height, 144u);
}

TEST(SlotAllocatorTest, FullPageFailsAndLeavesItUnchanged) {
  AtlasOptions o;
  o.width = 288;  // Two 144 columns.
  o.max_height = 288;
  SlotAllocator a(o);
  std::vector<Slot> slots;
  for (int i = 0; i < 4; ++i) slots.push_back(MustAlloc(&a, 128, 128));
  EXPECT_EQ(a.page_size().height, 288u);
  const AtlasStats before = a.stats();

  Slot out;
  out.id = 12345;
  EXPECT_EQ(a.Alloc(128, 128, &out), WT_ERR_LIMIT);
  EXPECT_EQ(out.id, 12345u);  // Untouched.
  EXPECT_EQ(a.Alloc(16, 16, &out), WT_ERR_LIMIT);
  const AtlasStats after = a.stats();
  EXPECT_EQ(after.live_slots, before.live_slots);
  EXPECT_EQ(after.page.height, before.page.height);

  // Freeing makes room again.
  ASSERT_EQ(a.Free(slots[2].id), WT_OK);
  EXPECT_EQ(a.Alloc(128, 128, &out), WT_OK);
  EXPECT_TRUE(Same(out.outer, slots[2].outer));
}

TEST(SlotAllocatorTest, MaxHeightCapsTheShelfStack) {
  AtlasOptions o;
  o.max_height = 256;
  SlotAllocator a(o);
  MustAlloc(&a, 128, 128);  // 144
  Slot out;
  EXPECT_EQ(a.Alloc(64, 64, &out), WT_OK);         // 80 more: 224
  EXPECT_EQ(a.Alloc(96, 96, &out), WT_ERR_LIMIT);  // 112 more: 336 > 256
  EXPECT_EQ(a.page_size().height, 224u);
}

TEST(SlotAllocatorTest, RejectsBucketsThatCanNeverFit) {
  AtlasOptions o;
  o.width = 512;
  o.max_height = 256;
  SlotAllocator a(o);
  Slot out;
  EXPECT_EQ(a.Alloc(0, 10, &out), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(a.Alloc(10, 0, &out), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(a.Alloc(10, 10, nullptr), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(a.Alloc(509, 10, &out), WT_ERR_INVALID_ARGUMENT);  // 513 + 15
  EXPECT_EQ(a.Alloc(10, 253, &out), WT_ERR_INVALID_ARGUMENT);  // 257 -> 272
  EXPECT_EQ(a.Alloc(UINT32_MAX, UINT32_MAX, &out), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(a.Alloc(UINT32_MAX, 10, &out), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(a.Alloc(10, UINT32_MAX, &out), WT_ERR_INVALID_ARGUMENT);
  // The largest that does fit: 508 + 4 = 512 wide, 252 + 4 = 256 tall.
  EXPECT_EQ(a.Alloc(508, 252, &out), WT_OK);
  EXPECT_EQ(out.outer.width, 512u);
  EXPECT_EQ(out.outer.height, 256u);
}

TEST(SlotAllocatorTest, StaleAndBogusHandlesAreRejected) {
  SlotAllocator a;
  Slot out;
  EXPECT_EQ(a.Free(wt::kInvalidSlot), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(a.Free(0x00010001u), WT_ERR_INVALID_ARGUMENT);  // Nothing yet.
  EXPECT_EQ(a.Get(wt::kInvalidSlot, &out), WT_ERR_INVALID_ARGUMENT);

  const Slot s = MustAlloc(&a, 128, 128);
  EXPECT_EQ(a.Get(s.id, nullptr), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(a.Free(s.id + 1), WT_ERR_INVALID_ARGUMENT);  // Another index.
  EXPECT_EQ(a.Free(s.id + 0x10000u), WT_ERR_INVALID_ARGUMENT);  // Generation.
  ASSERT_EQ(a.Free(s.id), WT_OK);
  EXPECT_EQ(a.Free(s.id), WT_ERR_INVALID_ARGUMENT);  // Double free.
  EXPECT_EQ(a.Get(s.id, &out), WT_ERR_INVALID_ARGUMENT);

  // The index is reused, under a new generation: the old handle stays dead.
  const Slot t = MustAlloc(&a, 128, 128);
  EXPECT_NE(t.id, s.id);
  EXPECT_EQ(a.Free(s.id), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(a.stats().live_slots, 1u);
}

TEST(SlotAllocatorTest, ClearFreesEverythingAndInvalidatesHandles) {
  SlotAllocator a;
  std::vector<Slot> slots;
  for (int i = 0; i < 40; ++i) slots.push_back(MustAlloc(&a, 128, 128));
  a.Clear();
  EXPECT_EQ(a.page_size().height, 0u);
  EXPECT_EQ(a.stats().live_slots, 0u);
  Slot out;
  for (const Slot& s : slots) {
    EXPECT_EQ(a.Get(s.id, &out), WT_ERR_INVALID_ARGUMENT);
  }
  const Slot again = MustAlloc(&a, 128, 128);
  EXPECT_TRUE(Same(again.outer, {0, 0, 144, 144}));
  a.Clear();
  a.Clear();  // Idempotent.
  EXPECT_EQ(a.stats().live_slots, 0u);
}

TEST(SlotAllocatorTest, OptionsAreClamped) {
  AtlasOptions o;
  o.width = 5000;
  o.max_height = 3;
  o.gutter = 0;
  SlotAllocator a(o);
  EXPECT_EQ(a.options().width, 2048u);
  EXPECT_EQ(a.options().max_height, 16u);
  EXPECT_EQ(a.options().gutter, 2u);
  EXPECT_EQ(a.page_size().width, 2048u);

  o.width = 1000;  // Rounded down to 992.
  o.max_height = 1000;
  o.gutter = 1000;
  SlotAllocator b(o);
  EXPECT_EQ(b.options().width, 992u);
  EXPECT_EQ(b.options().max_height, 992u);
  EXPECT_EQ(b.options().gutter, 64u);
}

TEST(SlotAllocatorTest, WiderGuttersMoveTheContent) {
  AtlasOptions o;
  o.gutter = 8;
  SlotAllocator a(o);
  const Slot s = MustAlloc(&a, 128, 128);
  // 128 + 16 = 144.
  EXPECT_TRUE(Same(s.outer, {0, 0, 144, 144}));
  EXPECT_TRUE(Same(s.inner, {8, 8, 128, 128}));
  const Slot t = MustAlloc(&a, 129, 129);  // 145 -> 160.
  EXPECT_EQ(t.outer.width, 160u);
  EXPECT_EQ(t.inner.x, t.outer.x + 8);
}

TEST(SlotAllocatorTest, CompactableRowsCountsWhatARepackWouldSave) {
  SlotAllocator a;
  // Two shelves of 128 buckets, one slot each.
  std::vector<Slot> slots;
  for (int i = 0; i < 15; ++i) slots.push_back(MustAlloc(&a, 128, 128));
  EXPECT_EQ(a.page_size().height, 288u);
  EXPECT_EQ(a.stats().compactable_rows, 0u);
  // Free 13 of the 14 on shelf 0: both shelves are nearly empty, but 2 slots
  // fit on one.
  for (int i = 0; i < 13; ++i) {
    ASSERT_EQ(a.Free(slots[static_cast<size_t>(i)].id), WT_OK);
  }
  EXPECT_EQ(a.stats().live_slots, 2u);
  EXPECT_EQ(a.stats().compactable_rows, 144u);
  EXPECT_EQ(a.stats().free_slots, 26u);
}

// --- Randomized alloc/free ---------------------------------------------------

struct Live {
  Slot slot;
  uint32_t bucket_w = 0;
  uint32_t bucket_h = 0;
};

struct Config {
  uint32_t width;
  uint32_t max_height;
  uint32_t gutter;
};

class PropertyTest {
 public:
  PropertyTest(const Config& cfg, uint32_t seed)
      : opts_{cfg.width, cfg.max_height, cfg.gutter}, a_(opts_), rng_(seed) {}

  // Runs `ops` random operations, checking everything after each, then frees
  // all. Returns a digest of every outcome, to compare runs.
  uint64_t Run(int ops) {
    uint64_t digest = 1469598103934665603ull;
    auto mix = [&digest](uint64_t v) {
      digest = (digest ^ v) * 1099511628211ull;
    };
    for (int i = 0; i < ops; ++i) {
      // Alternate fill-heavy and drain-heavy phases so the page both hits its
      // limit and empties out.
      const uint32_t alloc_percent = (i / 400) % 2 == 0 ? 85 : 25;
      const uint32_t roll = Next(100);
      if (roll < alloc_percent || live_.empty()) {
        mix(DoAlloc());
      } else if (roll < 97) {
        mix(DoFree());
      } else {
        mix(DoBadFree());
      }
      if (::testing::Test::HasFatalFailure()) return digest;
      CheckAll(i);
      if (::testing::Test::HasFailure()) return digest;
      mix(a_.page_size().height);
      mix(a_.stats().compactable_rows);
    }
    while (!ids_.empty()) {
      mix(DoFree());
      CheckAll(-1);
      if (::testing::Test::HasFailure()) return digest;
    }
    EXPECT_EQ(a_.page_size().height, 0u);
    const AtlasStats st = a_.stats();
    EXPECT_EQ(st.live_slots, 0u);
    EXPECT_EQ(st.shelves, 0u);
    EXPECT_EQ(st.empty_shelves, 0u);
    EXPECT_EQ(st.empty_rows, 0u);
    EXPECT_EQ(st.free_slots, 0u);
    return digest;
  }

  int full_failures() const { return full_failures_; }
  size_t peak_live() const { return peak_live_; }
  uint32_t peak_height() const { return peak_height_; }

 private:
  uint32_t Next(uint32_t n) { return static_cast<uint32_t>(rng_() % n); }

  std::pair<uint32_t, uint32_t> PickBucket() {
    static const std::pair<uint32_t, uint32_t> kBuckets[] = {
        {64, 64},  {96, 96},   {128, 128}, {128, 96}, {96, 128},
        {100, 60}, {160, 160}, {256, 256}, {30, 200}, {16, 16},
        {1, 1},    {512, 512}, {124, 124}, {125, 1},  {300, 40},
    };
    return kBuckets[Next(sizeof(kBuckets) / sizeof(kBuckets[0]))];
  }

  uint64_t DoAlloc() {
    const auto [w, h] = PickBucket();
    const AtlasStats before = a_.stats();
    const uint32_t free_before = a_.FreeSlotsFor(w, h);
    const uint32_t pw = RoundUp16(w + 2 * opts_.gutter);
    const uint32_t ph = RoundUp16(h + 2 * opts_.gutter);
    const bool fits = pw <= a_.options().width && ph <= a_.options().max_height;
    Slot s;
    const wt_status st = a_.Alloc(w, h, &s);
    if (!fits) {
      EXPECT_EQ(st, WT_ERR_INVALID_ARGUMENT) << w << "x" << h;
      return 1000;
    }
    if (st == WT_ERR_LIMIT) {
      ++full_failures_;
      // Genuinely full: no free column for the bucket, no empty shelf tall
      // enough, no room under max_height for another shelf.
      EXPECT_EQ(free_before, 0u);
      EXPECT_LT(before.largest_empty_rows, ph);
      EXPECT_GT(before.page.height + ph, a_.options().max_height);
      // And nothing changed.
      const AtlasStats after = a_.stats();
      EXPECT_EQ(after.page.height, before.page.height);
      EXPECT_EQ(after.live_slots, before.live_slots);
      EXPECT_EQ(after.free_slots, before.free_slots);
      EXPECT_EQ(after.empty_rows, before.empty_rows);
      return 2000;
    }
    EXPECT_EQ(st, WT_OK) << w << "x" << h;
    if (st != WT_OK) return 3000;
    EXPECT_EQ(s.outer.width, pw);
    EXPECT_EQ(s.outer.height, ph);
    EXPECT_EQ(a_.stats().live_slots, before.live_slots + 1);
    Live l;
    l.slot = s;
    l.bucket_w = w;
    l.bucket_h = h;
    EXPECT_EQ(live_.count(s.id), 0u);
    live_[s.id] = l;
    ids_.push_back(s.id);
    peak_live_ = std::max(peak_live_, live_.size());
    peak_height_ = std::max(peak_height_, a_.page_size().height);
    return (static_cast<uint64_t>(s.outer.x) << 32) | s.outer.y;
  }

  uint64_t DoFree() {
    const size_t i = Next(static_cast<uint32_t>(ids_.size()));
    const SlotId id = ids_[i];
    ids_[i] = ids_.back();
    ids_.pop_back();
    EXPECT_EQ(a_.Free(id), WT_OK);
    const Live l = live_[id];
    live_.erase(id);
    dead_.push_back(id);
    return (static_cast<uint64_t>(l.slot.outer.x) << 32) | l.slot.outer.y;
  }

  uint64_t DoBadFree() {
    if (dead_.empty()) return 0;
    const SlotId id = dead_[Next(static_cast<uint32_t>(dead_.size()))];
    // A freed handle stays dead unless its index was reused, and then a
    // different generation is in force.
    if (live_.count(id) == 0) {
      EXPECT_EQ(a_.Free(id), WT_ERR_INVALID_ARGUMENT);
    }
    return 4000;
  }

  void CheckAll(int op) {
    SCOPED_TRACE(testing::Message() << "after op " << op);
    const wt::AtlasPageSize page = a_.page_size();
    const AtlasStats st = a_.stats();
    ASSERT_EQ(page.width, a_.options().width);
    ASSERT_LE(page.width, 2048u);
    ASSERT_LE(page.height, a_.options().max_height);
    ASSERT_LE(page.height, 2048u);
    ASSERT_EQ(page.height % 16, 0u);
    ASSERT_EQ(st.page.height, page.height);

    std::vector<const Live*> rects;
    uint32_t bottom = 0;
    uint64_t area = 0;
    // Shelf (outer.y) -> the bucket and pitch it holds, and how many slots.
    struct ShelfModel {
      uint32_t bw, bh, pw, ph, count;
    };
    std::map<uint32_t, ShelfModel> shelves;
    for (const auto& kv : live_) {
      const Live& l = kv.second;
      const AtlasRect& o = l.slot.outer;
      const AtlasRect& in = l.slot.inner;
      Slot got;
      ASSERT_EQ(a_.Get(kv.first, &got), WT_OK);
      ASSERT_TRUE(Same(got.outer, o) && Same(got.inner, in))
          << "slots must not move";
      ASSERT_EQ(o.x % 16, 0u);
      ASSERT_EQ(o.y % 16, 0u);
      ASSERT_EQ(o.width % 16, 0u);
      ASSERT_EQ(o.height % 16, 0u);
      ASSERT_LE(o.x + o.width, page.width);
      ASSERT_LE(o.y + o.height, page.height);
      ASSERT_EQ(in.width, l.bucket_w);
      ASSERT_EQ(in.height, l.bucket_h);
      ASSERT_GE(in.x - o.x, 2u);
      ASSERT_GE(in.y - o.y, 2u);
      ASSERT_GE((o.x + o.width) - (in.x + in.width), 2u);
      ASSERT_GE((o.y + o.height) - (in.y + in.height), 2u);
      ASSERT_EQ(in.x - o.x, opts_.gutter > 64 ? 64u : opts_.gutter);
      bottom = std::max(bottom, o.y + o.height);
      area += static_cast<uint64_t>(o.width) * o.height;
      rects.push_back(&l);

      auto it = shelves.find(o.y);
      if (it == shelves.end()) {
        shelves[o.y] = {l.bucket_w, l.bucket_h, o.width, o.height, 1};
      } else {
        ShelfModel& m = it->second;
        ASSERT_TRUE(m.bw == l.bucket_w && m.bh == l.bucket_h &&
                    m.pw == o.width && m.ph == o.height)
            << "a shelf holds one bucket";
        ++m.count;
      }
    }

    // No overlaps: sweep by y.
    std::sort(rects.begin(), rects.end(), [](const Live* p, const Live* q) {
      return p->slot.outer.y < q->slot.outer.y;
    });
    for (size_t i = 0; i < rects.size(); ++i) {
      const AtlasRect& r = rects[i]->slot.outer;
      for (size_t j = i + 1; j < rects.size(); ++j) {
        const AtlasRect& s = rects[j]->slot.outer;
        if (s.y >= r.y + r.height) break;
        ASSERT_FALSE(Overlap(r, s))
            << "(" << r.x << "," << r.y << " " << r.width << "x" << r.height
            << ") overlaps (" << s.x << "," << s.y << " " << s.width << "x"
            << s.height << ")";
      }
    }

    // Shelves don't overlap each other either: same y or disjoint rows.
    uint32_t prev_end = 0;
    uint32_t covered = 0;
    for (const auto& kv : shelves) {
      ASSERT_GE(kv.first, prev_end);
      prev_end = kv.first + kv.second.ph;
      covered += kv.second.ph;
    }

    // The page is exactly as tall as the lowest live slot's bottom.
    ASSERT_EQ(page.height, bottom);

    // Stats against the model.
    ASSERT_EQ(st.live_slots, live_.size());
    ASSERT_EQ(st.live_area, area);
    ASSERT_EQ(st.unused_area,
              static_cast<uint64_t>(page.width) * page.height - area);
    ASSERT_EQ(st.shelves, shelves.size());
    ASSERT_EQ(st.empty_rows, page.height - covered);
    ASSERT_LE(st.largest_empty_rows, st.empty_rows);
    ASSERT_EQ(st.empty_rows == 0, st.empty_shelves == 0);
    ASSERT_LE(st.empty_shelves, st.empty_rows / 16);
    uint32_t free_slots = 0;
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>>
        buckets;  // bucket -> (live, columns); pitch height below
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> bucket_ph;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> bucket_free;
    for (const auto& kv : shelves) {
      const ShelfModel& m = kv.second;
      const uint32_t cols = page.width / m.pw;
      ASSERT_LE(m.count, cols);
      free_slots += cols - m.count;
      const auto key = std::make_pair(m.bw, m.bh);
      buckets[key].first += m.count;
      buckets[key].second = cols;
      bucket_ph[key] = m.ph;
      bucket_free[key] += cols - m.count;
    }
    ASSERT_EQ(st.free_slots, free_slots);
    uint32_t dense = 0;
    for (const auto& kv : buckets) {
      ASSERT_EQ(a_.FreeSlotsFor(kv.first.first, kv.first.second),
                bucket_free[kv.first]);
      dense += (kv.second.first + kv.second.second - 1) / kv.second.second *
               bucket_ph[kv.first];
    }
    ASSERT_EQ(st.compactable_rows, page.height - dense);
  }

  AtlasOptions opts_;
  SlotAllocator a_;
  std::mt19937 rng_;
  std::map<SlotId, Live> live_;
  std::vector<SlotId> ids_;
  std::vector<SlotId> dead_;
  int full_failures_ = 0;
  size_t peak_live_ = 0;
  uint32_t peak_height_ = 0;
};

TEST(SlotAllocatorPropertyTest, RandomAllocFreeKeepsEveryInvariant) {
  const Config configs[] = {
      {2048, 2048, 2},  // The real thing.
      {2048, 512, 2},   // Capped page: hits the limit constantly.
      {512, 512, 2},   {256, 1024, 2},  // Few columns, many shelves.
      {288, 288, 2},                    // Tiny: two columns of 128-buckets.
      {1024, 640, 8},                   // Wide gutters.
      {16, 2048, 2},  // One 16-px column: only 1x1..12x12 buckets fit.
  };
  int total_full = 0;
  for (const Config& cfg : configs) {
    for (uint32_t seed = 1; seed <= 12; ++seed) {
      SCOPED_TRACE(testing::Message()
                   << "page " << cfg.width << "x" << cfg.max_height
                   << " gutter " << cfg.gutter << " seed " << seed);
      PropertyTest t(cfg, seed * 7919);
      t.Run(1500);
      if (::testing::Test::HasFailure()) return;
      total_full += t.full_failures();
    }
  }
  // The test must actually exercise the full-page path.
  EXPECT_GT(total_full, 100);
}

TEST(SlotAllocatorPropertyTest, ReachesDenseAndTallPages) {
  PropertyTest t({2048, 2048, 2}, 42);
  t.Run(6000);
  EXPECT_GT(t.peak_live(), 60u);
  EXPECT_GT(t.peak_height(), 700u);
}

TEST(SlotAllocatorPropertyTest, SameSeedSameLayout) {
  const Config cfg{2048, 1024, 2};
  PropertyTest a(cfg, 99);
  PropertyTest b(cfg, 99);
  PropertyTest c(cfg, 100);
  const uint64_t da = a.Run(2000);
  const uint64_t db = b.Run(2000);
  const uint64_t dc = c.Run(2000);
  EXPECT_EQ(da, db);
  EXPECT_NE(da, dc);
}

}  // namespace
