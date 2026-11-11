// Tests for the atlas compositor (T4.2): slots and page sizes, presents into
// recycled buffers (every live slot rewritten, gutters cleared, one present
// per tick), targets coming and going, switching players between their own
// sink and the atlas, and the C ABI on a real animation.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "anim_util.h"
#include "atlas/compositor.h"
#include "atlas_util.h"
#include "gtest/gtest.h"
#include "sched/scheduler.h"
#include "sched_util.h"
#include "wt_api.h"

namespace {

using std::chrono::milliseconds;
using wt_test::FakeSink;
using wt_test::FakeTarget;
using wt_test::Pattern;
using wt_test::PatternSource;
using wt_test::PresentChecker;
using wt_test::Tags;

constexpr auto kWait = milliseconds(3000);

// Presents of a page start at least a period, less the scheduler's 1 ms of
// timer slack, apart.
std::chrono::microseconds MinGap(uint32_t fps) {
  return std::chrono::microseconds(1000000 / fps) - milliseconds(1);
}

wt_atlas_options Options(uint32_t width, uint32_t max_height,
                         uint32_t max_pages = 4) {
  wt_atlas_options o;
  wt_atlas_options_init_default(&o);
  o.width = width;
  o.max_height = max_height;
  o.max_pages = max_pages;
  return o;
}

// A scheduler, an atlas on it and the present checker, torn down in order.
class AtlasTest : public ::testing::Test {
 protected:
  void Start(const wt_atlas_options& options, uint32_t fps = 60,
             uint32_t workers = 2) {
    wt_sched_options so{workers, fps, 0};
    ASSERT_EQ(wt_sched_create(&so, nullptr, &sched_), WT_OK);
    impl_ = reinterpret_cast<wt::Scheduler*>(sched_);
    checker_ = std::make_unique<PresentChecker>(&tags_, MinGap(fps));
    ASSERT_EQ(wt_atlas_create(sched_, &options, &atlas_), WT_OK);
    Impl()->SetObserverForTesting(checker_.get());
  }

  void TearDown() override {
    wt_atlas_destroy(atlas_);
    for (wt::Player* p : players_) impl_->RemovePlayer(p);
    wt_sched_destroy(sched_);
    if (checker_ != nullptr) {
      EXPECT_EQ(checker_->failures(), 0);
    }
  }

  wt::Atlas* Impl() { return reinterpret_cast<wt::Atlas*>(atlas_); }

  // Sinks and targets live as long as the fixture: the scheduler may call
  // them until TearDown destroyed it.
  FakeSink& Home(uint32_t w, uint32_t h) {
    sinks_.push_back(std::make_unique<FakeSink>(w, h, /*fake_pixels=*/false));
    return *sinks_.back();
  }
  FakeTarget& Target(uint32_t w, uint32_t h, uint32_t count = 4,
                     uint32_t pad_px = 0) {
    targets_.push_back(std::make_unique<FakeTarget>(w, h, count, pad_px));
    return *targets_.back();
  }

  // A looping pattern player of w x h with `frames` frames of `ms` each,
  // drawing into *home (a sink of its own when null) until attached.
  wt::Player* Add(uint32_t tag, uint32_t w, uint32_t h, FakeSink* home,
                  uint32_t frames = 4, uint32_t ms = 20) {
    if (home == nullptr) home = &Home(w, h);
    auto log = std::make_shared<PatternSource::Log>();
    logs_.push_back(log);
    std::unique_ptr<wt::FrameSource> src = std::make_unique<PatternSource>(
        tag, w, h, std::vector<uint32_t>(frames, ms), log);
    wt::Player* p = nullptr;
    EXPECT_EQ(impl_->AddPlayer(&src, true, home->sink(), &p), WT_OK);
    tags_.Set(p, tag);
    players_.push_back(p);
    return p;
  }

  void Remove(wt::Player* p) {
    const uint32_t tag = tags_.Get(p);
    impl_->RemovePlayer(p);
    tags_.Erase(p, tag);
    players_.erase(std::find(players_.begin(), players_.end(), p));
  }

  wt_status Attach(wt::Player* p, wt_atlas_slot* slot) {
    return wt_atlas_attach(atlas_, reinterpret_cast<wt_player*>(p), slot);
  }
  wt_status Detach(wt::Player* p) {
    return wt_atlas_detach(atlas_, reinterpret_cast<wt_player*>(p));
  }
  wt_atlas_page_info Page(uint32_t i) {
    wt_atlas_page_info info;
    EXPECT_EQ(wt_atlas_get_page(atlas_, i, &info), WT_OK);
    return info;
  }

  wt_sched* sched_ = nullptr;
  wt::Scheduler* impl_ = nullptr;
  wt_atlas* atlas_ = nullptr;
  Tags tags_;
  std::unique_ptr<PresentChecker> checker_;
  std::vector<wt::Player*> players_;
  std::vector<std::shared_ptr<PatternSource::Log>> logs_;
  std::vector<std::unique_ptr<FakeSink>> sinks_;
  std::vector<std::unique_ptr<FakeTarget>> targets_;
};

// Waits until pred() holds.
template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout = kWait) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(milliseconds(2));
  }
  return pred();
}

TEST(AtlasOptionsTest, DefaultsAndValidation) {
  wt_atlas_options o;
  wt_atlas_options_init_default(&o);
  EXPECT_EQ(o.width, 2048u);
  EXPECT_EQ(o.max_height, 2048u);
  EXPECT_EQ(o.gutter, 2u);
  EXPECT_EQ(o.max_pages, 4u);
  EXPECT_EQ(o.format, WT_PIXEL_FORMAT_RGBA8888);
  wt_atlas_options_init_default(nullptr);

  wt_sched* sched = nullptr;
  ASSERT_EQ(wt_sched_create(nullptr, nullptr, &sched), WT_OK);
  wt_atlas* atlas = nullptr;
  for (auto bad : {&wt_atlas_options::width, &wt_atlas_options::max_height,
                   &wt_atlas_options::gutter, &wt_atlas_options::max_pages}) {
    wt_atlas_options b = o;
    b.*bad = 0;
    EXPECT_EQ(wt_atlas_create(sched, &b, &atlas), WT_ERR_INVALID_ARGUMENT);
    b.*bad = 4096;
    EXPECT_EQ(wt_atlas_create(sched, &b, &atlas), WT_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(atlas, nullptr);
  }
  wt_atlas_options b = o;
  b.format = 7;
  EXPECT_EQ(wt_atlas_create(sched, &b, &atlas), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_atlas_create(nullptr, &o, &atlas), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_atlas_create(sched, &o, nullptr), WT_ERR_INVALID_ARGUMENT);

  // Widths round down to 16; NULL options are the defaults.
  b = o;
  b.width = 100;
  ASSERT_EQ(wt_atlas_create(sched, &b, &atlas), WT_OK);
  EXPECT_EQ(reinterpret_cast<wt::Atlas*>(atlas)->options().width, 96u);
  EXPECT_EQ(wt_atlas_page_count(atlas), 0u);
  wt_atlas_page_info info;
  EXPECT_EQ(wt_atlas_get_page(atlas, 0, &info), WT_ERR_INVALID_ARGUMENT);
  wt_atlas_destroy(atlas);
  ASSERT_EQ(wt_atlas_create(sched, nullptr, &atlas), WT_OK);
  EXPECT_EQ(reinterpret_cast<wt::Atlas*>(atlas)->options().width, 2048u);
  wt_atlas_destroy(atlas);

  EXPECT_EQ(wt_atlas_page_count(nullptr), 0u);
  EXPECT_EQ(wt_atlas_attach(nullptr, nullptr, nullptr),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_atlas_detach(nullptr, nullptr), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_atlas_get_page(nullptr, 0, &info), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_atlas_set_target(nullptr, 0, nullptr, 0),
            WT_ERR_INVALID_ARGUMENT);
  wt_atlas_destroy(nullptr);
  wt_sched_destroy(sched);
}

TEST_F(AtlasTest, PresentsSlotsIntoStaleBuffersAndClearsGutters) {
  Start(Options(256, 256));
  FakeSink& home = Home(40, 40);
  wt::Player* p = Add(7, 40, 40, &home);
  ASSERT_TRUE(home.WaitShown(1, kWait));  // Standalone first.

  wt_atlas_slot slot;
  ASSERT_EQ(Attach(p, &slot), WT_OK);
  const int home_calls = home.acquires();
  EXPECT_EQ(slot.page, 0u);
  EXPECT_EQ(slot.x, 2u);
  EXPECT_EQ(slot.y, 2u);
  EXPECT_EQ(slot.width, 40u);
  EXPECT_EQ(slot.height, 40u);
  EXPECT_EQ(slot.outer_x, 0u);
  EXPECT_EQ(slot.outer_y, 0u);
  EXPECT_EQ(slot.outer_width, 48u);
  EXPECT_EQ(slot.outer_height, 48u);
  EXPECT_EQ(wt_atlas_page_count(atlas_), 1u);
  wt_atlas_page_info info = Page(0);
  EXPECT_EQ(info.width, 256u);
  EXPECT_EQ(info.used_height, 48u);
  EXPECT_EQ(info.wanted_height, 48u);
  EXPECT_EQ(info.height, 0u);
  EXPECT_EQ(info.has_target, 0u);
  EXPECT_EQ(info.live_slots, 1u);

  // Padded rows: the stride is not width * 4.
  FakeTarget& target = Target(256, 48, 3, /*pad_px=*/8);
  target.keep_copies(true);
  const wt_atlas_target t = target.target();
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 48), WT_OK);
  ASSERT_TRUE(target.WaitPresents(1, kWait));
  wt_player_set_active(reinterpret_cast<wt_player*>(p), true);
  ASSERT_TRUE(target.WaitPresents(12, kWait));
  EXPECT_EQ(home.acquires(), home_calls);  // Attached: never its own sink.

  // The checker saw every present; spot-check one buffer here too.
  const std::vector<uint8_t> last = target.last();
  uint32_t px;
  std::memcpy(&px, last.data() + 2 * target.stride() + 2 * 4, 4);
  EXPECT_EQ(px >> 12, Pattern(7, 0) >> 12);
  std::memcpy(&px, last.data() + 1 * target.stride() + 2 * 4, 4);
  EXPECT_EQ(px, 0u);  // Gutter row.
  std::memcpy(&px, last.data() + 2 * target.stride() + 47 * 4, 4);
  EXPECT_EQ(px, 0u);  // Right gutter.
  std::memcpy(&px, last.data() + 2 * target.stride() + 48 * 4, 4);
  EXPECT_EQ(px, 0xABABABABu);  // Outside every slot: never touched.

  info = Page(0);
  EXPECT_EQ(info.height, 48u);
  EXPECT_EQ(info.has_target, 1u);
  EXPECT_EQ(info.buffers, 3u);
  EXPECT_EQ(info.buffer_stride, target.stride());
  EXPECT_EQ(info.buffer_bytes, 3ull * target.stride() * 48);
  EXPECT_GE(info.cpu_bytes, 40u * 40 * 4);
  EXPECT_GE(info.presents, 12u);
  EXPECT_EQ(info.present_failures, 0u);
  EXPECT_GT(checker_->slots_checked(), 0u);

  // Destroying the atlas hands the player back to its own sink.
  wt_atlas_destroy(atlas_);
  atlas_ = nullptr;
  target.Retire();
  ASSERT_TRUE(WaitFor([&] { return home.acquires() > home_calls; }));
  EXPECT_EQ(target.late_calls(), 0);
  EXPECT_EQ(target.protocol_errors(), 0);
  EXPECT_EQ(target.overlaps(), 0);
  EXPECT_EQ(home.protocol_errors(), 0);
}

TEST_F(AtlasTest, PagesGrowShrinkAndEmpty) {
  Start(Options(96, 1024));  // Two 48-px slots per shelf.
  std::vector<wt::Player*> ps;
  for (uint32_t i = 0; i < 20; ++i) {
    ps.push_back(Add(100 + i, 40, 40, nullptr));
    wt_atlas_slot slot;
    ASSERT_EQ(Attach(ps.back(), &slot), WT_OK);
    EXPECT_EQ(slot.outer_x, (i % 2) * 48);
    EXPECT_EQ(slot.outer_y, (i / 2) * 48);
  }
  wt_atlas_page_info info = Page(0);
  EXPECT_EQ(info.used_height, 480u);
  EXPECT_EQ(info.wanted_height, 480u);
  FakeTarget& big = Target(96, 480);
  wt_atlas_target t = big.target();
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 480), WT_OK);
  ASSERT_TRUE(big.WaitPresents(1, kWait));

  // A few rows free: keep the buffer.
  ASSERT_EQ(Detach(ps[19]), WT_OK);
  ASSERT_EQ(Detach(ps[18]), WT_OK);
  info = Page(0);
  EXPECT_EQ(info.used_height, 432u);
  EXPECT_EQ(info.wanted_height, 480u);
  // At least max(128, 480 / 4) rows free: shrink.
  for (int i = 17; i >= 2; --i) {
    ASSERT_EQ(Detach(ps[i]), WT_OK);
  }
  info = Page(0);
  EXPECT_EQ(info.used_height, 48u);
  EXPECT_EQ(info.wanted_height, 48u);
  EXPECT_EQ(info.live_slots, 2u);
  FakeTarget& small = Target(96, 48);
  t = small.target();
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 48), WT_OK);
  big.Retire();
  ASSERT_TRUE(small.WaitPresents(1, kWait));
  EXPECT_EQ(Page(0).wanted_height, 48u);

  // Freed columns are reused without growing.
  wt_atlas_slot slot;
  ASSERT_EQ(Detach(ps[1]), WT_OK);
  ASSERT_EQ(Attach(ps[5], &slot), WT_OK);
  EXPECT_EQ(slot.outer_y, 0u);
  EXPECT_EQ(Page(0).wanted_height, 48u);

  // Empty: the page wants no target.
  ASSERT_EQ(Detach(ps[0]), WT_OK);
  ASSERT_EQ(Detach(ps[5]), WT_OK);
  info = Page(0);
  EXPECT_EQ(info.used_height, 0u);
  EXPECT_EQ(info.wanted_height, 0u);
  EXPECT_EQ(info.cpu_bytes, 0u);
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, nullptr, 0), WT_OK);
  small.Retire();
  info = Page(0);
  EXPECT_EQ(info.has_target, 0u);
  EXPECT_EQ(info.buffer_bytes, 0u);
  EXPECT_EQ(Detach(ps[0]), WT_ERR_INVALID_ARGUMENT);  // Not attached.
  EXPECT_EQ(big.late_calls() + small.late_calls(), 0);
}

TEST_F(AtlasTest, AddsPagesUntilFull) {
  Start(Options(64, 64, /*max_pages=*/2));  // One 48-px slot per page.
  FakeSink& home = Home(40, 40);
  wt::Player* a = Add(1, 40, 40, nullptr);
  wt::Player* b = Add(2, 40, 40, nullptr);
  wt::Player* c = Add(3, 40, 40, &home);
  wt_atlas_slot slot;
  ASSERT_EQ(Attach(a, &slot), WT_OK);
  EXPECT_EQ(slot.page, 0u);
  ASSERT_EQ(Attach(b, &slot), WT_OK);
  EXPECT_EQ(slot.page, 1u);
  EXPECT_EQ(Attach(c, &slot), WT_ERR_LIMIT);
  EXPECT_EQ(wt_atlas_page_count(atlas_), 2u);
  // c still plays standalone.
  const int before = home.acquires();
  wt_player_seek(reinterpret_cast<wt_player*>(c), 1);
  EXPECT_TRUE(WaitFor([&] { return home.acquires() > before; }));

  // Freed space on page 0 is used first.
  ASSERT_EQ(Detach(a), WT_OK);
  ASSERT_EQ(Attach(c, &slot), WT_OK);
  EXPECT_EQ(slot.page, 0u);

  // Sizes that can never fit, and players that can't be attached.
  wt::Player* wide = Add(4, 61, 8, nullptr);
  EXPECT_EQ(Attach(wide, &slot), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(Attach(b, &slot), WT_ERR_INVALID_ARGUMENT);  // Already in.
  wt_atlas* other = nullptr;
  ASSERT_EQ(wt_atlas_create(sched_, nullptr, &other), WT_OK);
  EXPECT_EQ(wt_atlas_attach(other, reinterpret_cast<wt_player*>(b), &slot),
            WT_ERR_INVALID_ARGUMENT);  // In another atlas.
  EXPECT_EQ(wt_atlas_detach(other, reinterpret_cast<wt_player*>(b)),
            WT_ERR_INVALID_ARGUMENT);
  wt_atlas_destroy(other);
  std::unique_ptr<wt::FrameSource> fake = std::make_unique<wt_test::FakeSource>(
      std::vector<uint32_t>{10}, std::make_shared<wt_test::FakeSource::Log>());
  wt::Player* unsized = nullptr;
  ASSERT_EQ(impl_->AddPlayer(&fake, true, Home(4, 4).sink(), &unsized), WT_OK);
  players_.push_back(unsized);
  EXPECT_EQ(Attach(unsized, &slot), WT_ERR_INVALID_ARGUMENT);  // No size.
  wt_sched* sched2 = nullptr;
  ASSERT_EQ(wt_sched_create(nullptr, nullptr, &sched2), WT_OK);
  wt_atlas* foreign = nullptr;
  ASSERT_EQ(wt_atlas_create(sched2, nullptr, &foreign), WT_OK);
  EXPECT_EQ(wt_atlas_attach(foreign, reinterpret_cast<wt_player*>(a), &slot),
            WT_ERR_INVALID_ARGUMENT);  // Another scheduler.
  wt_atlas_destroy(foreign);
  wt_sched_destroy(sched2);

  // Bad targets.
  FakeTarget& target = Target(64, 48);
  wt_atlas_target t = target.target();
  EXPECT_EQ(wt_atlas_set_target(atlas_, 2, &t, 48), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_atlas_set_target(atlas_, 0, &t, 40), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_atlas_set_target(atlas_, 0, &t, 0), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_atlas_set_target(atlas_, 0, &t, 80), WT_ERR_INVALID_ARGUMENT);
  t.present = nullptr;
  EXPECT_EQ(wt_atlas_set_target(atlas_, 0, &t, 48), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(target.acquires(), 0);
}

TEST_F(AtlasTest, PausedPagePresentsIntoEachNewTarget) {
  Start(Options(160, 256));
  std::vector<wt::Player*> ps;
  for (uint32_t i = 0; i < 3; ++i) {
    ps.push_back(Add(20 + i, 40, 40, nullptr, /*frames=*/5));
    wt_atlas_slot slot;
    ASSERT_EQ(Attach(ps.back(), &slot), WT_OK);
  }
  FakeTarget& a = Target(160, 48);
  wt_atlas_target t = a.target();
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 48), WT_OK);
  ASSERT_TRUE(WaitFor([&] {
    return checker_->shown(ps[0]) == 0 && checker_->shown(ps[1]) == 0 &&
           checker_->shown(ps[2]) == 0;
  }));
  for (wt::Player* p : ps) {
    ASSERT_EQ(wt_player_seek(reinterpret_cast<wt_player*>(p), 3), WT_OK);
  }
  ASSERT_TRUE(WaitFor([&] {
    return checker_->shown(ps[0]) == 3 && checker_->shown(ps[1]) == 3 &&
           checker_->shown(ps[2]) == 3;
  }));

  // Nothing plays, yet a new target gets the whole page at once, from the
  // slots' CPU frames.
  const uint64_t draws = logs_[0]->draws + logs_[1]->draws + logs_[2]->draws;
  FakeTarget& b = Target(160, 64, 2);
  b.keep_copies(true);
  t = b.target();
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 64), WT_OK);
  a.Retire();
  ASSERT_TRUE(b.WaitPresents(1, kWait));
  EXPECT_EQ(logs_[0]->draws + logs_[1]->draws + logs_[2]->draws, draws);
  const std::vector<uint8_t> last = b.last();
  for (uint32_t i = 0; i < 3; ++i) {
    uint32_t px;
    std::memcpy(&px, last.data() + 10 * b.stride() + (i * 48 + 10) * 4, 4);
    EXPECT_EQ(px, Pattern(20 + i, 3));
  }

  // No target: nothing is presented, and the next one gets everything.
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, nullptr, 64), WT_OK);
  b.Retire();
  EXPECT_EQ(Page(0).has_target, 0u);
  EXPECT_EQ(Page(0).height, 64u);
  for (wt::Player* p : ps) {
    ASSERT_EQ(wt_player_seek(reinterpret_cast<wt_player*>(p), 4), WT_OK);
  }
  ASSERT_TRUE(WaitFor([&] {
    return logs_[0]->last_drawn == 4 && logs_[1]->last_drawn == 4 &&
           logs_[2]->last_drawn == 4;
  }));
  FakeTarget& c = Target(160, 48);
  t = c.target();
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 48), WT_OK);
  ASSERT_TRUE(WaitFor([&] {
    return checker_->shown(ps[0]) == 4 && checker_->shown(ps[1]) == 4 &&
           checker_->shown(ps[2]) == 4;
  }));
  EXPECT_EQ(a.late_calls() + b.late_calls(), 0);
}

TEST_F(AtlasTest, BusyAndMissingTargets) {
  Start(Options(64, 64));
  FakeSink& home = Home(40, 40);
  wt::Player* p = Add(9, 40, 40, &home, 8, 10);
  wt_atlas_slot slot;
  ASSERT_EQ(Attach(p, &slot), WT_OK);
  FakeTarget& target = Target(64, 48);
  target.set_busy_for(3);
  const wt_atlas_target t = target.target();
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 48), WT_OK);
  ASSERT_TRUE(target.WaitPresents(1, kWait));  // Retried after WT_BUSY.
  EXPECT_GE(target.acquires(), 4);

  // WT_NO_TARGET parks the page until a target is set again, playing or not.
  target.set_answer(WT_NO_TARGET);
  wt_player_set_active(reinterpret_cast<wt_player*>(p), true);
  ASSERT_TRUE(WaitFor([&] { return target.acquires() > 4; }));
  std::this_thread::sleep_for(milliseconds(50));
  const int parked = target.acquires();
  std::this_thread::sleep_for(milliseconds(150));
  EXPECT_EQ(target.acquires(), parked);
  target.set_answer(WT_OK);
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 48), WT_OK);
  const int presents = target.presents();
  ASSERT_TRUE(target.WaitPresents(presents + 5, kWait));

  // An error retries after about 100 ms and is counted.
  target.set_answer(WT_ERR_IO);
  ASSERT_TRUE(WaitFor([&] { return Page(0).present_failures >= 2; }));
  target.set_answer(WT_OK);
  ASSERT_TRUE(target.WaitPresents(target.presents() + 2, kWait));
}

TEST_F(AtlasTest, PlayersSwitchBetweenOwnSinkAndAtlas) {
  Start(Options(128, 128));
  FakeSink& home = Home(40, 40);
  wt::Player* p = Add(11, 40, 40, &home, 4, 10);
  wt_player_set_active(reinterpret_cast<wt_player*>(p), true);
  FakeTarget& target = Target(128, 48);
  const wt_atlas_target t = target.target();
  for (int round = 0; round < 5; ++round) {
    const size_t shown = home.shown_count();
    ASSERT_TRUE(home.WaitShown(shown + 2, kWait));
    wt_atlas_slot slot;
    ASSERT_EQ(Attach(p, &slot), WT_OK);
    const int calls = home.acquires();
    if (round == 0) {
      ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 48), WT_OK);
    }
    const int presents = target.presents();
    ASSERT_TRUE(target.WaitPresents(presents + 3, kWait));
    EXPECT_EQ(home.acquires(), calls);
    ASSERT_EQ(Detach(p), WT_OK);
    EXPECT_EQ(Page(0).live_slots, 0u);
  }
  EXPECT_EQ(home.protocol_errors(), 0);
  EXPECT_EQ(home.overlaps(), 0);
}

TEST_F(AtlasTest, DestroyingAnAttachedPlayerFreesItsSlot) {
  Start(Options(128, 128));
  FakeSink& home_a = Home(40, 40);
  FakeSink& home_b = Home(40, 40);
  wt::Player* a = Add(30, 40, 40, &home_a, 4, 10);
  wt::Player* b = Add(31, 40, 40, &home_b, 4, 10);
  wt_atlas_slot slot;
  ASSERT_EQ(Attach(a, &slot), WT_OK);
  ASSERT_EQ(Attach(b, &slot), WT_OK);
  FakeTarget& target = Target(128, 48);
  const wt_atlas_target t = target.target();
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, 48), WT_OK);
  wt_player_set_active(reinterpret_cast<wt_player*>(a), true);
  wt_player_set_active(reinterpret_cast<wt_player*>(b), true);
  ASSERT_TRUE(target.WaitPresents(5, kWait));
  Remove(a);
  home_a.MarkDestroyed();
  EXPECT_EQ(Page(0).live_slots, 1u);
  const int presents = target.presents();
  ASSERT_TRUE(target.WaitPresents(presents + 5, kWait));
  Remove(b);
  home_b.MarkDestroyed();
  EXPECT_EQ(Page(0).live_slots, 0u);
  EXPECT_EQ(Page(0).wanted_height, 0u);
  EXPECT_EQ(home_a.late_calls() + home_b.late_calls(), 0);
}

TEST_F(AtlasTest, PresentsAtMostOncePerTick) {
  constexpr uint32_t kFps = 30;
  Start(Options(512, 512), kFps, 4);
  // Twenty players at 100 fps with different phases: rounds keep finding
  // some of them due.
  std::vector<wt::Player*> ps;
  for (uint32_t i = 0; i < 20; ++i) {
    ps.push_back(Add(40 + i, 24, 24, nullptr, 10, 10));
    wt_atlas_slot slot;
    ASSERT_EQ(Attach(ps.back(), &slot), WT_OK);
  }
  const wt_atlas_page_info info = Page(0);
  FakeTarget& target = Target(512, info.wanted_height);
  const wt_atlas_target t = target.target();
  ASSERT_EQ(wt_atlas_set_target(atlas_, 0, &t, info.wanted_height), WT_OK);
  const auto start = std::chrono::steady_clock::now();
  for (wt::Player* p : ps) {
    wt_player_set_active(reinterpret_cast<wt_player*>(p), true);
    std::this_thread::sleep_for(milliseconds(3));
  }
  std::this_thread::sleep_for(milliseconds(600));
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  // The checker enforces the gap; here the total, plus one for the target.
  EXPECT_LE(target.presents(), static_cast<int>(elapsed * kFps) + 2);
  EXPECT_GE(target.presents(), static_cast<int>(elapsed * kFps / 2));
}

// The C ABI end to end on a real VP9 animation with alpha: the slot shows
// exactly the frame wt_anim_next_frame decodes, in either pixel format.
TEST(AtlasCAbiTest, RealAnimationMatchesDecodedFrames) {
  for (wt_pixel_format format :
       {WT_PIXEL_FORMAT_RGBA8888, WT_PIXEL_FORMAT_BGRA8888}) {
    constexpr uint32_t kSize = 32;
    wt_sched* sched = nullptr;
    ASSERT_EQ(wt_sched_create(nullptr, nullptr, &sched), WT_OK);
    wt_atlas_options o = Options(256, 256);
    o.format = format;
    o.gutter = 4;
    wt_atlas* atlas = nullptr;
    ASSERT_EQ(wt_atlas_create(sched, &o, &atlas), WT_OK);

    wt_test::Anim anim;
    ASSERT_EQ(wt_test::OpenAnim("vp9_alpha_vfr.webm", kSize, kSize, &anim),
              WT_OK);
    FakeSink home(kSize, kSize, false);
    const wt_sink sink = home.sink();
    wt_player* player = nullptr;
    ASSERT_EQ(wt_player_create(sched, anim.p, true, &sink, &player), WT_OK);
    anim.p = nullptr;  // The player owns it.
    wt_atlas_slot slot;
    ASSERT_EQ(wt_atlas_attach(atlas, player, &slot), WT_OK);
    EXPECT_EQ(slot.x - slot.outer_x, 4u);
    EXPECT_EQ(slot.width, kSize);
    wt_atlas_page_info info;
    ASSERT_EQ(wt_atlas_get_page(atlas, slot.page, &info), WT_OK);
    FakeTarget target(256, info.wanted_height, 3, 0, format);
    target.keep_copies(true);
    const wt_atlas_target t = target.target();
    ASSERT_EQ(wt_atlas_set_target(atlas, slot.page, &t, info.wanted_height),
              WT_OK);

    wt_test::Anim ref;
    ASSERT_EQ(wt_test::OpenAnim("vp9_alpha_vfr.webm", kSize, kSize, &ref),
              WT_OK);
    for (uint32_t frame : {2u, 5u, 0u}) {
      ASSERT_EQ(wt_player_seek(player, frame), WT_OK);
      ASSERT_EQ(wt_anim_seek(ref.p, frame), WT_OK);
      wt_test::Bytes expected;
      ASSERT_EQ(wt_test::Next(ref, &expected, format), WT_OK);
      // Wait for a present showing it (the slot's first row matches).
      bool match = false;
      const auto end = std::chrono::steady_clock::now() + kWait;
      while (!match && std::chrono::steady_clock::now() < end) {
        const std::vector<uint8_t> last = target.last();
        if (!last.empty()) {
          match = true;
          for (uint32_t y = 0; y < kSize && match; ++y) {
            match =
                std::memcmp(last.data() + size_t{slot.y + y} * target.stride() +
                                size_t{slot.x} * 4,
                            expected.data() + size_t{y} * kSize * 4,
                            kSize * 4) == 0;
          }
        }
        if (!match) std::this_thread::sleep_for(milliseconds(5));
      }
      EXPECT_TRUE(match) << "frame " << frame << " format " << format;
    }
    // Destroying the player while attached frees its slot.
    wt_player_destroy(player);
    ASSERT_EQ(wt_atlas_get_page(atlas, slot.page, &info), WT_OK);
    EXPECT_EQ(info.live_slots, 0u);
    wt_atlas_destroy(atlas);
    target.Retire();
    wt_sched_destroy(sched);
    EXPECT_EQ(target.late_calls(), 0);
    EXPECT_EQ(target.protocol_errors(), 0);
  }
}

}  // namespace
