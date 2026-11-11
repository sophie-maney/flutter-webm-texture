// The T4.2 stress test: animations churn through atlases while the scheduler
// plays them. Four drivers each own two small atlases (so pages fill, grow,
// shrink, empty and new pages appear) and up to 30 players, and keep
// creating, destroying, attaching, detaching and moving players between
// their own sink and either atlas, playing, pausing, seeking and redrawing
// them, losing and restoring page surfaces, and destroying and recreating an
// atlas; a tuner keeps changing the worker count and max_concurrent. A fake
// glue gives every page a target of exactly its wanted height, replacing it
// whenever that changes.
//
// Checked throughout: every present writes every live slot that fits its
// buffer with exactly its player's latest frame, and clears every gutter,
// although each buffer comes back full of stale pixels; a page presents at
// most once per tick (the checker's minimum gap); no target, slot or own
// sink is called after it was replaced or destroyed, nor out of protocol;
// a player's own sink is never called while it is in an atlas. At the end,
// with everything paused, the last present of every slot shows the last
// frame its player drew (no present was lost).
//
// Built with -DWT_TSAN=ON it is the race check; with -DWT_SANITIZE=ON, the
// lifetime check.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
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

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using wt_test::FakeSink;
using wt_test::FakeTarget;
using wt_test::PatternSource;
using wt_test::PresentChecker;
using wt_test::Tags;

constexpr int kDrivers = 4;
constexpr int kAtlasesPerDriver = 2;
constexpr int kMaxAlive = 30;
constexpr int kRealEvery = 12;  // Every 12th player decodes a real file.
constexpr auto kRunFor = milliseconds(4000);
constexpr uint32_t kFps = 120;

struct Bucket {
  uint32_t w, h;
};
constexpr Bucket kBuckets[] = {{12, 12}, {20, 20}, {28, 12},
                               {12, 28}, {40, 40}, {24, 24}};

// Presents of a page start at least a period, less the scheduler's 1 ms of
// timer slack, apart.
std::chrono::microseconds MinGap() {
  return std::chrono::microseconds(1000000 / kFps) - milliseconds(1);
}

struct Counters {
  std::atomic<int> created{0}, attaches{0}, detaches{0}, moves{0};
  std::atomic<int> limits{0}, grows{0}, shrinks{0}, releases{0};
  std::atomic<int> losses{0}, recreates{0}, destroyed_attached{0};
  std::atomic<int> failures{0}, max_pages{0};
  std::atomic<uint64_t> presents{0}, slots_checked{0};
};

struct Live {
  wt_player* player = nullptr;
  uint32_t frames = 0;
  uint32_t w = 0, h = 0;
  std::unique_ptr<FakeSink> home;
  std::shared_ptr<PatternSource::Log> log;  // Null for a real animation.
  uint32_t tag = UINT32_MAX;                // Pattern tag (UINT32_MAX: real).
  int atlas = -1;        // Index of the driver's atlas it is in, or -1.
  int home_calls = 0;    // home->acquires() when it entered the atlas.
  wt_atlas_slot slot{};  // While in an atlas.
};

struct GluePage {
  std::shared_ptr<FakeTarget> target;
  uint32_t height = 0;
  bool lost = false;
};

struct GlueAtlas {
  wt_atlas* atlas = nullptr;
  uint32_t width = 0;
  std::unique_ptr<PresentChecker> checker;
  std::vector<GluePage> pages;
};

class Driver {
 public:
  Driver(int index, wt_sched* sched, Tags* tags, Counters* c,
         wt_source* real_source)
      : index_(index),
        sched_(sched),
        impl_(reinterpret_cast<wt::Scheduler*>(sched)),
        tags_(tags),
        c_(c),
        real_source_(real_source),
        rng_(static_cast<uint32_t>(index * 7919 + 1)) {}

  void Run(Clock::time_point start) {
    for (GlueAtlas& ga : atlases_) Create(&ga);
    bool recreated = false;
    while (Clock::now() - start < kRunFor) {
      Step();
      if (!recreated && Clock::now() - start > kRunFor / 2) {
        Recreate(&atlases_[rng_() % kAtlasesPerDriver]);
        recreated = true;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(rng_() % 1500));
    }
  }

  // Everything paused, every page has a target of its wanted height.
  void Settle() {
    for (Live& l : alive_) {
      wt_player_set_active(l.player, false);
      l.home->set_answer(WT_OK);
    }
    for (GlueAtlas& ga : atlases_) {
      for (size_t i = 0; i < ga.pages.size(); ++i) Restore(&ga, i);
      Sync(&ga);
    }
  }

  // After the scheduler went idle: the last present of every slot shows
  // the last frame its player drew.
  void CheckFinal() {
    for (const Live& l : alive_) {
      if (l.atlas < 0 || l.log == nullptr) continue;
      const wt::Player* p = reinterpret_cast<const wt::Player*>(l.player);
      const uint32_t drawn = l.log->last_drawn.load();
      const uint32_t shown = atlases_[l.atlas].checker->shown(p);
      if (drawn != shown) {
        const GlueAtlas& ga = atlases_[l.atlas];
        wt_atlas_page_info info;
        wt_atlas_get_page(ga.atlas, l.slot.page, &info);
        const GluePage& gp = ga.pages[l.slot.page];
        std::fprintf(
            stderr,
            "driver %d: slot shows %u, player drew %u (draws %u); page %u "
            "outer y %u h %u; page used %u wanted %u height %u target %u "
            "presents %llu failures %llu; glue height %u lost %d target %p "
            "presents %d acquires %d\n",
            index_, shown, drawn, l.log->draws.load(), l.slot.page,
            l.slot.outer_y, l.slot.outer_height, info.used_height,
            info.wanted_height, info.height, info.has_target,
            static_cast<unsigned long long>(info.presents),
            static_cast<unsigned long long>(info.present_failures), gp.height,
            gp.lost, static_cast<void*>(gp.target.get()),
            gp.target ? gp.target->presents() : -1,
            gp.target ? gp.target->acquires() : -1);
        c_->failures++;
      }
    }
  }

  void Finish() {
    while (!alive_.empty()) Destroy(alive_.size() - 1);
    for (GlueAtlas& ga : atlases_) Teardown(&ga);
  }

  uint64_t presents() const {
    uint64_t n = 0;
    for (const GlueAtlas& ga : atlases_) n += ga.checker->presents();
    return n;
  }

 private:
  void Fail(const char* what) {
    std::fprintf(stderr, "driver %d: %s\n", index_, what);
    c_->failures++;
  }

  void Create(GlueAtlas* ga) {
    wt_atlas_options o;
    wt_atlas_options_init_default(&o);
    const uint32_t widths[] = {96, 128, 160};
    // Up to 256 rows, so pages can free max(128, height / 4) rows and
    // shrink.
    const uint32_t heights[] = {64, 256, 256};
    o.width = widths[rng_() % 3];
    o.max_height = heights[rng_() % 3];
    o.max_pages = 1 + rng_() % 3;
    o.gutter = 2 + rng_() % 2;
    if (wt_atlas_create(sched_, &o, &ga->atlas) != WT_OK) Fail("create");
    ga->width = o.width;
    ga->checker = std::make_unique<PresentChecker>(tags_, MinGap());
    reinterpret_cast<wt::Atlas*>(ga->atlas)->SetObserverForTesting(
        ga->checker.get());
    ga->pages.clear();
  }

  void Teardown(GlueAtlas* ga) {
    // Players in it go back to their own sinks.
    for (Live& l : alive_) {
      if (l.atlas >= 0 && &atlases_[l.atlas] == ga) {
        CheckHomeQuiet(l);
        l.atlas = -1;
      }
    }
    wt_atlas_destroy(ga->atlas);
    ga->atlas = nullptr;
    for (GluePage& gp : ga->pages) {
      if (gp.target != nullptr) Retire(gp.target);
    }
    ga->pages.clear();
    if (ga->checker->failures() != 0) Fail("present checks failed");
    c_->presents += ga->checker->presents();
    c_->slots_checked += ga->checker->slots_checked();
  }

  void Recreate(GlueAtlas* ga) {
    Teardown(ga);
    Create(ga);
    c_->recreates++;
  }

  void Retire(const std::shared_ptr<FakeTarget>& t) {
    t->Retire();
    retired_.push_back(t);
  }

  std::shared_ptr<FakeTarget> NewTarget(const GlueAtlas& ga, uint32_t h) {
    return std::make_shared<FakeTarget>(ga.width, h, 2 + rng_() % 5,
                                        /*pad_px=*/(rng_() % 2) * 4);
  }

  // The glue's job after any change: every page gets a target of exactly
  // its wanted height (none when it wants 0).
  void Sync(GlueAtlas* ga) {
    const uint32_t n = wt_atlas_page_count(ga->atlas);
    c_->max_pages.store(
        std::max<int>(c_->max_pages.load(), static_cast<int>(n)));
    ga->pages.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      GluePage& gp = ga->pages[i];
      if (gp.lost) continue;  // Restored later, at the wanted height.
      wt_atlas_page_info info;
      if (wt_atlas_get_page(ga->atlas, i, &info) != WT_OK) Fail("get_page");
      if (info.wanted_height == gp.height) continue;
      std::shared_ptr<FakeTarget> next;
      if (info.wanted_height > 0) {
        next = NewTarget(*ga, info.wanted_height);
        const wt_atlas_target t = next->target();
        if (wt_atlas_set_target(ga->atlas, i, &t, info.wanted_height) !=
            WT_OK) {
          Fail("set_target");
        }
        (info.wanted_height > gp.height ? c_->grows : c_->shrinks)++;
      } else {
        if (wt_atlas_set_target(ga->atlas, i, nullptr, 0) != WT_OK) {
          Fail("set_target(NULL)");
        }
        c_->releases++;
      }
      if (gp.target != nullptr) Retire(gp.target);
      gp.target = next;
      gp.height = info.wanted_height;
    }
  }

  // The surface went away (the app went to the background).
  void Lose(GlueAtlas* ga, size_t i) {
    GluePage& gp = ga->pages[i];
    if (gp.lost || gp.target == nullptr) return;
    if (wt_atlas_set_target(ga->atlas, static_cast<uint32_t>(i), nullptr,
                            gp.height) != WT_OK) {
      Fail("lose");
    }
    Retire(gp.target);
    gp.target = nullptr;
    gp.lost = true;
    c_->losses++;
  }

  // The surface is back: a target of the height the page wants now.
  void Restore(GlueAtlas* ga, size_t i) {
    GluePage& gp = ga->pages[i];
    if (!gp.lost) return;
    gp.lost = false;
    wt_atlas_page_info info;
    if (wt_atlas_get_page(ga->atlas, static_cast<uint32_t>(i), &info) !=
        WT_OK) {
      Fail("get_page");
    }
    std::shared_ptr<FakeTarget> next;
    wt_status s;
    if (info.wanted_height > 0) {
      next = NewTarget(*ga, info.wanted_height);
      const wt_atlas_target t = next->target();
      s = wt_atlas_set_target(ga->atlas, static_cast<uint32_t>(i), &t,
                              info.wanted_height);
    } else {
      s = wt_atlas_set_target(ga->atlas, static_cast<uint32_t>(i), nullptr, 0);
    }
    if (s != WT_OK) Fail("restore");
    gp.target = next;
    gp.height = info.wanted_height;
  }

  // A player's own sink must not be called while it is in an atlas.
  void CheckHomeQuiet(const Live& l) {
    if (l.home->acquires() != l.home_calls) Fail("own sink called in atlas");
  }

  void NewPlayer() {
    const int id = c_->created.fetch_add(1);
    Live l;
    const Bucket b = kBuckets[rng_() % (sizeof(kBuckets) / sizeof(Bucket))];
    l.w = b.w;
    l.h = b.h;
    l.home = std::make_unique<FakeSink>(b.w, b.h, /*fake_pixels=*/false);
    const wt_sink sink = l.home->sink();
    const bool loop = rng_() % 4 != 0;
    if (id % kRealEvery == 0) {
      wt_anim* anim = nullptr;
      if (wt_anim_create(real_source_, b.w, b.h, &anim) != WT_OK) {
        Fail("anim");
        return;
      }
      wt_info info;
      wt_anim_get_info(anim, &info, nullptr, nullptr);
      l.frames = info.frame_count;
      if (wt_player_create(sched_, anim, loop, &sink, &l.player) != WT_OK) {
        wt_anim_destroy(anim);
        Fail("player");
        return;
      }
    } else {
      std::vector<uint32_t> durations(1 + rng_() % 30);
      for (uint32_t& d : durations) d = rng_() % 40;
      l.frames = static_cast<uint32_t>(durations.size());
      l.log = std::make_shared<PatternSource::Log>();
      std::unique_ptr<wt::FrameSource> src = std::make_unique<PatternSource>(
          static_cast<uint32_t>(id), b.w, b.h, std::move(durations), l.log);
      wt::Player* p = nullptr;
      if (impl_->AddPlayer(&src, loop, sink, &p) != WT_OK) {
        Fail("AddPlayer");
        return;
      }
      l.player = reinterpret_cast<wt_player*>(p);
      l.tag = static_cast<uint32_t>(id);
      tags_->Set(p, l.tag);
    }
    wt_player_set_active(l.player, rng_() % 3 != 0);
    alive_.push_back(std::move(l));
  }

  void Attach(Live* l, int k) {
    GlueAtlas& ga = atlases_[k];
    wt_atlas_slot slot;
    const wt_status s = wt_atlas_attach(ga.atlas, l->player, &slot);
    if (s == WT_ERR_LIMIT) {
      c_->limits++;
      return;
    }
    if (s != WT_OK) {
      Fail("attach");
      return;
    }
    if (slot.width != l->w || slot.height != l->h ||
        slot.x < slot.outer_x + 2 || slot.y < slot.outer_y + 2 ||
        slot.x + slot.width + 2 > slot.outer_x + slot.outer_width ||
        slot.y + slot.height + 2 > slot.outer_y + slot.outer_height) {
      Fail("slot rects");
    }
    l->atlas = k;
    l->slot = slot;
    l->home_calls = l->home->acquires();
    c_->attaches++;
    Sync(&ga);
  }

  void Detach(Live* l) {
    CheckHomeQuiet(*l);
    GlueAtlas& ga = atlases_[l->atlas];
    if (wt_atlas_detach(ga.atlas, l->player) != WT_OK) Fail("detach");
    ga.checker->Forget(reinterpret_cast<const wt::Player*>(l->player));
    l->atlas = -1;
    c_->detaches++;
    Sync(&ga);
  }

  void Destroy(size_t i) {
    Live& l = alive_[i];
    const int k = l.atlas;
    if (k >= 0) {
      CheckHomeQuiet(l);
      c_->destroyed_attached++;
    }
    wt_player_destroy(l.player);
    l.home->MarkDestroyed();
    const wt::Player* p = reinterpret_cast<const wt::Player*>(l.player);
    tags_->Erase(p, l.tag);
    if (k >= 0) {
      atlases_[k].checker->Forget(p);
      Sync(&atlases_[k]);
    }
    if (l.home->late_calls() != 0 || l.home->protocol_errors() != 0 ||
        l.home->overlaps() != 0) {
      Fail("own sink protocol");
    }
    if (l.log != nullptr && l.log->exclusive.overlaps() != 0) {
      Fail("source overlap");
    }
    alive_.erase(alive_.begin() + static_cast<std::ptrdiff_t>(i));
  }

  void Step() {
    const int roll = static_cast<int>(rng_() % 100);
    if (roll < 20 && static_cast<int>(alive_.size()) < kMaxAlive) {
      NewPlayer();
      return;
    }
    if (alive_.empty()) return;
    const size_t i = rng_() % alive_.size();
    Live& l = alive_[i];
    const int op = static_cast<int>(rng_() % 100);
    if (op < 22) {
      if (l.atlas < 0) {
        Attach(&l, static_cast<int>(rng_() % kAtlasesPerDriver));
      } else {
        Detach(&l);
      }
    } else if (op < 30) {
      if (l.atlas >= 0) {  // Move to the other atlas (or back in).
        const int k =
            (l.atlas + 1 + static_cast<int>(rng_() % 2)) % kAtlasesPerDriver;
        Detach(&l);
        Attach(&l, k);
        c_->moves++;
      }
    } else if (op < 52) {
      wt_player_set_active(l.player, rng_() % 3 != 0);
    } else if (op < 60) {
      wt_player_set_priority(l.player, rng_() % 1000);
    } else if (op < 70) {
      if (wt_player_seek(l.player, rng_() % l.frames) != WT_OK) Fail("seek");
    } else if (op < 75) {
      wt_player_redraw(l.player);
    } else if (op < 80) {
      if (l.atlas < 0) {  // Its own target misbehaves only standalone.
        const wt_status answers[] = {WT_OK, WT_OK, WT_NO_TARGET, WT_BUSY,
                                     WT_ERR_IO};
        l.home->set_answer(answers[rng_() % 5]);
      }
    } else if (op < 86) {
      GlueAtlas& ga = atlases_[rng_() % kAtlasesPerDriver];
      if (!ga.pages.empty()) {
        const size_t p = rng_() % ga.pages.size();
        if (ga.pages[p].lost) {
          Restore(&ga, p);
        } else {
          Lose(&ga, p);
        }
      }
    } else if (op < 92) {
      Destroy(i);
    }
  }

  const int index_;
  wt_sched* const sched_;
  wt::Scheduler* const impl_;
  Tags* const tags_;
  Counters* const c_;
  wt_source* const real_source_;
  std::mt19937 rng_;
  std::vector<Live> alive_;
  GlueAtlas atlases_[kAtlasesPerDriver];

 public:
  // Every target this driver replaced, checked at the very end.
  std::vector<std::shared_ptr<FakeTarget>> retired_;
};

// Waits until a count stops changing for `quiet`.
template <typename Count>
void WaitQuiet(Count count, milliseconds quiet, milliseconds timeout) {
  const auto end = Clock::now() + timeout;
  auto last = count();
  auto since = Clock::now();
  while (Clock::now() < end) {
    std::this_thread::sleep_for(milliseconds(10));
    const auto now = count();
    if (now != last) {
      last = now;
      since = Clock::now();
    } else if (Clock::now() - since >= quiet) {
      return;
    }
  }
}

TEST(AtlasStressTest, ChurnAcrossAtlasesPagesAndTargets) {
  wt_sched_options options{4, kFps, 0};
  wt_sched* sched = nullptr;
  ASSERT_EQ(wt_sched_create(&options, nullptr, &sched), WT_OK);
  wt_test::Source real_source;
  ASSERT_EQ(wt_test::OpenSource("vp9_alpha_vfr.webm", &real_source), WT_OK);

  Tags tags;
  Counters c;
  std::vector<std::unique_ptr<Driver>> drivers;
  for (int i = 0; i < kDrivers; ++i) {
    drivers.push_back(
        std::make_unique<Driver>(i, sched, &tags, &c, real_source.p));
  }

  std::atomic<bool> churning{true};
  std::thread tuner([&] {
    std::mt19937 rng(4321);
    const uint32_t caps[] = {0, 0, 4, 16};
    while (churning.load()) {
      // The frame rate stays put: the checkers' minimum gap depends on it.
      wt_sched_options o{1 + static_cast<uint32_t>(rng() % 4), kFps,
                         caps[rng() % 4]};
      if (wt_sched_set_options(sched, &o) != WT_OK) c.failures++;
      std::this_thread::sleep_for(milliseconds(rng() % 80));
    }
    wt_sched_options o{4, kFps, 0};
    if (wt_sched_set_options(sched, &o) != WT_OK) c.failures++;
  });

  const Clock::time_point start = Clock::now();
  {
    std::vector<std::thread> threads;
    for (auto& d : drivers) {
      threads.emplace_back([&d, start] { d->Run(start); });
    }
    for (std::thread& t : threads) t.join();
  }
  churning = false;
  tuner.join();

  // Quiesce and check that nothing drawn went unpresented.
  for (auto& d : drivers) d->Settle();
  auto activity = [&] {
    wt_sched_stats s;
    wt_sched_get_stats(sched, &s);
    uint64_t presents = 0;
    for (auto& d : drivers) presents += d->presents();
    return s.frames_drawn + presents;
  };
  WaitQuiet(activity, milliseconds(300), milliseconds(10000));
  for (auto& d : drivers) d->CheckFinal();
  for (auto& d : drivers) d->Finish();

  wt_sched_stats stats;
  ASSERT_EQ(wt_sched_get_stats(sched, &stats), WT_OK);
  wt_sched_destroy(sched);

  int late = 0;
  int protocol = 0;
  for (auto& d : drivers) {
    for (const auto& t : d->retired_) {
      late += t->late_calls();
      protocol += t->protocol_errors() + t->overlaps();
    }
  }
  EXPECT_EQ(c.failures.load(), 0);
  EXPECT_EQ(late, 0);
  EXPECT_EQ(protocol, 0);
  EXPECT_EQ(stats.players, 0u);
  EXPECT_GT(c.created.load(), 100);
  EXPECT_GT(c.attaches.load(), 150);
  EXPECT_GT(c.detaches.load(), 75);
  EXPECT_GT(c.moves.load(), 10);
  EXPECT_GT(c.grows.load(), 20);
  EXPECT_GT(c.shrinks.load(), 0);
  EXPECT_GT(c.releases.load(), 5);
  EXPECT_GT(c.losses.load(), 10);
  EXPECT_GT(c.destroyed_attached.load(), 10);
  EXPECT_GE(c.recreates.load(), kDrivers);
  EXPECT_GE(c.max_pages.load(), 2);
  EXPECT_GT(c.presents.load(), 300u);
  EXPECT_GT(c.slots_checked.load(), 2000u);
  std::printf(
      "atlas stress: %d players, %d attaches, %d detaches, %d moves, "
      "%d limits, %d grows, %d shrinks, %d releases, %d surface losses, "
      "%d recreates, max %d pages, %llu presents, %llu slots checked, "
      "%llu frames drawn\n",
      c.created.load(), c.attaches.load(), c.detaches.load(), c.moves.load(),
      c.limits.load(), c.grows.load(), c.shrinks.load(), c.releases.load(),
      c.losses.load(), c.recreates.load(), c.max_pages.load(),
      static_cast<unsigned long long>(c.presents.load()),
      static_cast<unsigned long long>(c.slots_checked.load()),
      static_cast<unsigned long long>(stats.frames_drawn));
}

}  // namespace
