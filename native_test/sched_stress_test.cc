// The T3.3 stress test: 500 animations churn through one scheduler while
// eight threads create, destroy, start, stop, rank, seek and redraw them, and
// another keeps changing the options. Every sink checks the scheduler's
// promises (one callback at a time per player, release after acquire on the
// same thread, nothing after destroy). Built with -DWT_TSAN=ON it is the race
// check; with -DWT_SANITIZE=ON, the lifetime check.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "anim_util.h"
#include "gtest/gtest.h"
#include "sched/scheduler.h"
#include "sched_util.h"
#include "wt_api.h"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using wt_test::FakeSink;
using wt_test::FakeSource;

constexpr int kAnimations = 500;
constexpr int kDrivers = 8;
constexpr int kMaxAlivePerDriver = 25;  // Up to 200 alive at once.
constexpr int kRealEvery = 10;          // Every 10th decodes a real file.
constexpr auto kRunFor = milliseconds(4000);
constexpr uint32_t kRealSize = 32;

struct Live {
  wt_player* player = nullptr;
  uint32_t frames = 0;
  std::unique_ptr<FakeSink> sink;
  std::shared_ptr<FakeSource::Log> log;
};

TEST(SchedStressTest, FiveHundredAnimationsChurning) {
  wt_sched_options options{4, 120, 24};
  wt_sched* sched = nullptr;
  ASSERT_EQ(wt_sched_create(&options, nullptr, &sched), WT_OK);
  wt::Scheduler* impl = reinterpret_cast<wt::Scheduler*>(sched);

  const fs::path dir = fs::temp_directory_path() /
                       ("wt_sched_stress_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  wt_test::Source real_source;
  ASSERT_EQ(wt_test::OpenSource("vp9_alpha_vfr.webm", &real_source), WT_OK);

  std::atomic<int> created{0};
  std::atomic<int> failures{0};
  std::atomic<bool> drivers_done{false};
  std::atomic<int> alive_now{0};
  std::atomic<int> peak{0};
  std::mutex dead_mu;
  std::vector<Live> dead;  // Destroyed players' sinks, checked at the end.
  const Clock::time_point start = Clock::now();

  auto create = [&](int id, std::mt19937* rng) -> Live {
    Live live;
    if (id % kRealEvery == 0) {
      // A real animation, sharing three cache files with the others.
      const std::string path =
          (dir / ("c" + std::to_string(id % 3) + ".wsc")).string();
      wt_anim* anim = nullptr;
      if (wt_anim_create_cached(real_source.p, kRealSize, kRealSize,
                                path.c_str(), &anim) != WT_OK) {
        failures++;
        return live;
      }
      wt_info info;
      wt_anim_get_info(anim, &info, nullptr, nullptr);
      live.frames = info.frame_count;
      live.sink = std::make_unique<FakeSink>(kRealSize, kRealSize,
                                             /*fake_pixels=*/false);
      const wt_sink sink = live.sink->sink();
      if (wt_player_create(sched, anim, (*rng)() % 4 != 0, &sink,
                           &live.player) != WT_OK) {
        wt_anim_destroy(anim);
        failures++;
      }
      return live;
    }
    std::uniform_int_distribution<uint32_t> frames(1, 40);
    std::uniform_int_distribution<uint32_t> ms(0, 40);
    std::vector<uint32_t> durations(frames(*rng));
    for (uint32_t& d : durations) d = ms(*rng);
    live.frames = static_cast<uint32_t>(durations.size());
    live.log = std::make_shared<FakeSource::Log>();
    live.sink = std::make_unique<FakeSink>();
    std::unique_ptr<wt::FrameSource> src =
        std::make_unique<FakeSource>(std::move(durations), live.log);
    wt::Player* player = nullptr;
    if (impl->AddPlayer(&src, (*rng)() % 4 != 0, live.sink->sink(), &player) !=
        WT_OK) {
      failures++;
    }
    live.player = reinterpret_cast<wt_player*>(player);
    return live;
  };

  auto destroy = [&](Live live) {
    alive_now.fetch_sub(1);
    wt_player_destroy(live.player);
    live.sink->MarkDestroyed();
    std::lock_guard<std::mutex> lk(dead_mu);
    dead.push_back(std::move(live));
  };

  auto drive = [&](int seed) {
    std::mt19937 rng(static_cast<uint32_t>(seed));
    std::vector<Live> alive;
    for (;;) {
      const auto elapsed = Clock::now() - start;
      const bool over = elapsed >= kRunFor;
      if (over && created.load() >= kAnimations) break;
      // Spread the creations over the run.
      const int allowed =
          over ? kAnimations
               : static_cast<int>(kAnimations * elapsed / kRunFor) + kDrivers;
      const int roll = static_cast<int>(rng() % 100);
      if (static_cast<int>(alive.size()) < kMaxAlivePerDriver &&
          created.load() < allowed && roll < 40) {
        const int id = created.fetch_add(1);
        if (id >= kAnimations) {
          created.fetch_sub(1);
        } else {
          Live live = create(id, &rng);
          if (live.player != nullptr) {
            alive.push_back(std::move(live));
            const int n = alive_now.fetch_add(1) + 1;
            int p = peak.load();
            while (n > p && !peak.compare_exchange_weak(p, n)) {
            }
          }
        }
      } else if (!alive.empty()) {
        const size_t i = rng() % alive.size();
        Live& live = alive[i];
        const int op = static_cast<int>(rng() % 100);
        if (op < 30) {
          wt_player_set_active(live.player, rng() % 3 != 0);
        } else if (op < 45) {
          wt_player_set_priority(live.player, rng() % 1000);
        } else if (op < 60) {
          const uint32_t frame = static_cast<uint32_t>(rng() % live.frames);
          if (wt_player_seek(live.player, frame) != WT_OK) failures++;
          if (wt_player_seek(live.player, live.frames) !=
              WT_ERR_INVALID_ARGUMENT) {
            failures++;
          }
        } else if (op < 70) {
          live.sink->set_answer(WT_OK);
          wt_player_redraw(live.player);
        } else if (op < 75) {
          const int mode = static_cast<int>(rng() % 4);
          if (mode == 0) live.sink->set_answer(WT_NO_TARGET);
          if (mode == 1) live.sink->set_busy_for(3);
          if (mode == 2) live.sink->set_answer(WT_ERR_IO);
          if (mode == 3) live.sink->set_answer(WT_OK);
        } else if (op < 76 || over) {
          destroy(std::move(live));
          alive.erase(alive.begin() + static_cast<std::ptrdiff_t>(i));
        }
      }
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
    }
    for (Live& live : alive) destroy(std::move(live));
  };

  std::thread tuner([&] {
    std::mt19937 rng(1234);
    const uint32_t fps[] = {15, 30, 60, 120};
    const uint32_t caps[] = {0, 4, 16, 64};
    while (!drivers_done.load()) {
      wt_sched_options o{1 + static_cast<uint32_t>(rng() % 4), fps[rng() % 4],
                         caps[rng() % 4]};
      if (wt_sched_set_options(sched, &o) != WT_OK) failures++;
      wt_sched_stats stats;
      if (wt_sched_get_stats(sched, &stats) != WT_OK) failures++;
      std::this_thread::sleep_for(milliseconds(rng() % 60));
    }
  });
  std::vector<std::thread> drivers;
  for (int i = 0; i < kDrivers; ++i) drivers.emplace_back(drive, i + 1);
  for (std::thread& t : drivers) t.join();
  drivers_done = true;
  tuner.join();

  wt_sched_stats stats;
  ASSERT_EQ(wt_sched_get_stats(sched, &stats), WT_OK);
  wt_sched_destroy(sched);
  fs::remove_all(dir);

  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(created.load(), kAnimations);
  EXPECT_EQ(stats.players, 0u);
  EXPECT_GT(peak.load(), 100);
  EXPECT_GT(stats.frames_drawn, 5000u);
  ASSERT_EQ(dead.size(), static_cast<size_t>(kAnimations));
  size_t shown = 0;
  for (const Live& live : dead) {
    EXPECT_EQ(live.sink->overlaps(), 0);
    EXPECT_EQ(live.sink->protocol_errors(), 0);
    EXPECT_EQ(live.sink->late_calls(), 0);
    if (live.log != nullptr) {
      EXPECT_EQ(live.log->exclusive.overlaps(), 0);
    }
    shown += live.sink->shown_count();
  }
  EXPECT_GT(shown, 5000u);
  std::printf(
      "stress: %d animations, peak %d alive, %llu frames drawn, "
      "%llu skipped, %llu ticks\n",
      kAnimations, peak.load(),
      static_cast<unsigned long long>(stats.frames_drawn),
      static_cast<unsigned long long>(stats.frames_skipped),
      static_cast<unsigned long long>(stats.ticks));
}

}  // namespace
