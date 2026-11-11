// Tests for the scheduler (T3.3): the timing rules of wt::Timeline with a
// fake clock, the threaded wt::Scheduler with fake frame sources and sinks,
// and the C ABI (wt_sched_*, wt_player_*) on a real animation.
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "anim_util.h"
#include "api/anim.h"
#include "api/handles.h"
#include "gtest/gtest.h"
#include "sched/scheduler.h"
#include "sched/timeline.h"
#include "sched_util.h"
#include "wt_api.h"

namespace {

using std::chrono::milliseconds;
using wt::FrameJob;
using wt::FrameOutcome;
using wt::Nanos;
using wt::Timeline;
using wt::TimePoint;
using wt_test::FakeSink;
using wt_test::FakeSource;

using Kind = FrameJob::Kind;

std::vector<Nanos> Ms(std::initializer_list<int> ms) {
  std::vector<Nanos> out;
  for (int m : ms) out.push_back(milliseconds(m));
  return out;
}

std::vector<Nanos> Uniform(uint32_t frames, int ms) {
  return std::vector<Nanos>(frames, milliseconds(ms));
}

const TimePoint kT0 = TimePoint() + std::chrono::hours(1);
constexpr Nanos kEarly = milliseconds(8);
constexpr Nanos kStall = milliseconds(100);

// Completes `job` as if the frame had been drawn.
void Drawn(Timeline* t, const FrameJob& job, TimePoint done) {
  FrameOutcome out;
  out.drawn = true;
  out.cursor = job.target + 1;
  out.cursor_known = true;
  t->Complete(job, out, done);
}

// A timeline whose first frame is on screen, started at kT0.
Timeline Started(std::vector<Nanos> durations, bool loop = true) {
  Timeline t(std::move(durations), loop);
  Drawn(&t, t.PlanRequest(), kT0);
  t.Start(kT0);
  return t;
}

// --- Timeline
// -----------------------------------------------------------------

TEST(TimelineTest, StartsWithARequestForFrameZero) {
  Timeline t(Uniform(3, 40), true);
  EXPECT_TRUE(t.dirty());
  EXPECT_FALSE(t.running());
  const FrameJob job = t.PlanRequest();
  EXPECT_EQ(job.kind, Kind::kRequest);
  EXPECT_EQ(job.target, 0u);
  EXPECT_TRUE(job.seek);  // The cursor is unknown.
  Drawn(&t, job, kT0);
  EXPECT_FALSE(t.dirty());
  EXPECT_EQ(t.shown(), 0u);
}

TEST(TimelineTest, NothingPlaysWhileARequestIsPending) {
  Timeline t(Uniform(3, 40), true);
  t.Start(kT0);
  FrameJob job;
  EXPECT_FALSE(t.PlanPlayback(kT0 + milliseconds(500), kEarly, kStall, &job));
}

TEST(TimelineTest, PlaysOnTheContentClock) {
  Timeline t = Started(Ms({40, 20, 30}));
  EXPECT_EQ(t.due(), kT0 + milliseconds(40));
  FrameJob job;
  EXPECT_FALSE(t.PlanPlayback(kT0 + milliseconds(31), kEarly, kStall, &job));
  ASSERT_TRUE(t.PlanPlayback(kT0 + milliseconds(33), kEarly, kStall, &job));
  EXPECT_EQ(job.kind, Kind::kPlayback);
  EXPECT_EQ(job.target, 1u);
  EXPECT_FALSE(job.seek);
  EXPECT_EQ(job.skips, 0u);
  EXPECT_EQ(job.due, kT0 + milliseconds(40));
  // Finishing late doesn't shift the clock.
  Drawn(&t, job, kT0 + milliseconds(47));
  EXPECT_EQ(t.due(), kT0 + milliseconds(60));
}

TEST(TimelineTest, SkipsFramesThatAreAlreadyLate) {
  Timeline t = Started(Uniform(10, 20));
  FrameJob job;
  // Frames 1, 2 and 3 are due at +20, +40, +60; at +65 frame 3 is the one.
  ASSERT_TRUE(t.PlanPlayback(kT0 + milliseconds(65), kEarly, kStall, &job));
  EXPECT_EQ(job.target, 3u);
  EXPECT_EQ(job.due, kT0 + milliseconds(60));
  EXPECT_FALSE(job.seek);
  EXPECT_EQ(job.from, 1u);
  EXPECT_EQ(job.skips, 2u);
}

TEST(TimelineTest, CarriesOnAfterAStall) {
  Timeline t = Started(Uniform(10, 20));
  FrameJob job;
  const TimePoint now = kT0 + milliseconds(500);
  ASSERT_TRUE(t.PlanPlayback(now, kEarly, kStall, &job));
  EXPECT_EQ(job.target, 1u);
  EXPECT_EQ(job.due, now);
  EXPECT_EQ(job.skips, 0u);
}

TEST(TimelineTest, ContentFasterThanTheClockSkipsToKeepTime) {
  // 100 fps content, rounds every 50 ms (20 fps).
  Timeline t = Started(Uniform(100, 10));
  const Nanos period = milliseconds(50);
  TimePoint now = kT0;
  uint32_t drawn = 0;
  for (int round = 1; round <= 10; ++round) {
    now += period;
    FrameJob job;
    ASSERT_TRUE(t.PlanPlayback(now, period / 2, kStall, &job));
    Drawn(&t, job, now);
    ++drawn;
    // Within one frame of the content time.
    const auto content = (now - kT0) / milliseconds(10);
    EXPECT_NEAR(static_cast<double>(t.shown()), static_cast<double>(content),
                2.0)
        << "round " << round;
  }
  EXPECT_EQ(drawn, 10u);
}

TEST(TimelineTest, NeverLapsTheAnimationInOneJob) {
  Timeline t = Started(Uniform(4, 10));
  FrameJob job;
  ASSERT_TRUE(t.PlanPlayback(kT0 + milliseconds(95), kEarly, kStall, &job));
  EXPECT_LT(job.skips, 4u);
}

TEST(TimelineTest, LoopsThroughFrameZero) {
  Timeline t = Started(Uniform(3, 20));
  TimePoint now = kT0;
  for (uint32_t expect : {1u, 2u, 0u, 1u}) {
    now += milliseconds(20);
    FrameJob job;
    ASSERT_TRUE(t.PlanPlayback(now, kEarly, kStall, &job));
    EXPECT_EQ(job.target, expect);
    EXPECT_FALSE(job.seek) << "a loop restarts without seeking";
    FrameOutcome out;
    out.drawn = true;
    out.cursor = job.target + 1;  // 3 after the last frame: past the end.
    out.cursor_known = true;
    t.Complete(job, out, now);
  }
}

TEST(TimelineTest, NonLoopingEndsOnItsLastFrameUntilASeek) {
  Timeline t = Started(Uniform(3, 20), /*loop=*/false);
  TimePoint now = kT0;
  for (int i = 0; i < 2; ++i) {
    now += milliseconds(20);
    FrameJob job;
    ASSERT_TRUE(t.PlanPlayback(now, kEarly, kStall, &job));
    Drawn(&t, job, now);
  }
  EXPECT_EQ(t.shown(), 2u);
  FrameJob job;
  EXPECT_FALSE(t.PlanPlayback(now + milliseconds(100), kEarly, kStall, &job));
  EXPECT_TRUE(t.ended());
  EXPECT_FALSE(t.animates());
  t.Seek(0);
  EXPECT_FALSE(t.ended());
  EXPECT_TRUE(t.dirty());
  EXPECT_EQ(t.PlanRequest().target, 0u);
}

TEST(TimelineTest, StoppingKeepsTheRestOfTheFrame) {
  Timeline t = Started(Ms({40, 40}));
  t.Stop(kT0 + milliseconds(10));
  EXPECT_FALSE(t.running());
  t.Start(kT0 + milliseconds(1000));
  EXPECT_EQ(t.due(), kT0 + milliseconds(1030));
}

TEST(TimelineTest, ASeekWinsOverAJobInFlight) {
  Timeline t = Started(Uniform(10, 20));
  FrameJob job;
  ASSERT_TRUE(t.PlanPlayback(kT0 + milliseconds(20), kEarly, kStall, &job));
  t.Seek(7);
  Drawn(&t, job, kT0 + milliseconds(25));
  EXPECT_EQ(t.shown(), 7u);
  EXPECT_TRUE(t.dirty());
  // The cursor still follows the core: the request can tell it must seek.
  const FrameJob request = t.PlanRequest();
  EXPECT_EQ(request.target, 7u);
  EXPECT_TRUE(request.seek);
}

TEST(TimelineTest, ARequestThatFoundNoBufferNeedsNoSecondSeek) {
  Timeline t = Started(Uniform(10, 20));
  t.Seek(5);
  const FrameJob job = t.PlanRequest();
  EXPECT_TRUE(job.seek);
  FrameOutcome out;  // Positioned, but not drawn.
  out.cursor = 5;
  out.cursor_known = true;
  t.Complete(job, out, kT0);
  EXPECT_TRUE(t.dirty());
  EXPECT_FALSE(t.PlanRequest().seek);
}

TEST(TimelineTest, RedrawWhilePlayingMakesTheNextFrameDue) {
  Timeline t = Started(Uniform(10, 40));
  t.Redraw(kT0 + milliseconds(5));
  EXPECT_FALSE(t.dirty());
  FrameJob job;
  ASSERT_TRUE(t.PlanPlayback(kT0 + milliseconds(5), kEarly, kStall, &job));
  EXPECT_EQ(job.target, 1u);
}

TEST(TimelineTest, RedrawWhilePausedRequestsTheShownFrame) {
  Timeline t = Started(Uniform(10, 40));
  t.Stop(kT0 + milliseconds(5));
  t.Redraw(kT0 + milliseconds(6));
  EXPECT_TRUE(t.dirty());
  const FrameJob job = t.PlanRequest();
  EXPECT_EQ(job.target, 0u);
  EXPECT_TRUE(job.seek);  // Back to the frame already shown.
}

TEST(TimelineTest, RedrawOnTheLastFrameOfANonLoopingTimelineRequestsIt) {
  // Showing its last frame, playing, not yet marked ended.
  Timeline t = Started(Uniform(2, 40), /*loop=*/false);
  FrameJob job;
  ASSERT_TRUE(t.PlanPlayback(kT0 + milliseconds(40), kEarly, kStall, &job));
  Drawn(&t, job, kT0 + milliseconds(40));
  ASSERT_EQ(t.shown(), 1u);
  ASSERT_FALSE(t.ended());
  t.Redraw(kT0 + milliseconds(50));
  EXPECT_TRUE(t.dirty()) << "no next frame to stand in for the lost one";
  EXPECT_EQ(t.PlanRequest().target, 1u);
}

TEST(TimelineTest, AFrameThatFailedToDecodeStillUsesItsTurn) {
  Timeline t = Started(Uniform(10, 20));
  FrameJob job;
  ASSERT_TRUE(t.PlanPlayback(kT0 + milliseconds(20), kEarly, kStall, &job));
  FrameOutcome out;
  out.drawn = true;  // Decode failed, but the slot is used.
  out.cursor = 2;
  out.cursor_known = true;
  t.Complete(job, out, kT0 + milliseconds(21));
  EXPECT_EQ(t.shown(), 1u);
  EXPECT_EQ(t.due(), kT0 + milliseconds(40));
}

TEST(TimelineTest, BrokenDurationsAreRaised) {
  Timeline t = Started(Ms({0, 0, 0}));
  EXPECT_EQ(t.due(), kT0 + wt::kMinFrameTime);
}

// --- Scheduler (fake sources)
// -------------------------------------------------

struct Fixture {
  std::unique_ptr<wt::Scheduler> sched;

  explicit Fixture(uint32_t workers = 2, uint32_t max_fps = 60,
                   uint32_t max_concurrent = 0) {
    wt_sched_options o{workers, max_fps, max_concurrent};
    EXPECT_EQ(wt::Scheduler::Create(o, wt_sched_hooks{}, &sched), WT_OK);
  }

  struct Item {
    std::shared_ptr<FakeSource::Log> log = std::make_shared<FakeSource::Log>();
    std::unique_ptr<FakeSink> sink = std::make_unique<FakeSink>();
    wt::Player* player = nullptr;
  };

  // `answer` and `busy_for` configure the sink before the player exists, so
  // even its first acquire sees them.
  std::unique_ptr<Item> Add(std::vector<uint32_t> durations, bool loop = true,
                            wt_status answer = WT_OK, int busy_for = 0) {
    auto item = std::make_unique<Item>();
    item->sink->set_answer(answer);
    item->sink->set_busy_for(busy_for);
    std::unique_ptr<wt::FrameSource> src =
        std::make_unique<FakeSource>(std::move(durations), item->log);
    EXPECT_EQ(sched->AddPlayer(&src, loop, item->sink->sink(), &item->player),
              WT_OK);
    return item;
  }

  void Remove(Item* item) {
    sched->RemovePlayer(item->player);
    item->sink->MarkDestroyed();
  }

  wt_sched_stats Stats() const { return sched->Stats(); }
};

std::vector<uint32_t> Frames(uint32_t n, uint32_t ms) {
  return std::vector<uint32_t>(n, ms);
}

void Sleep(int ms) { std::this_thread::sleep_for(milliseconds(ms)); }

TEST(SchedulerTest, RejectsBadOptions) {
  std::unique_ptr<wt::Scheduler> s;
  EXPECT_EQ(wt::Scheduler::Create({0, 60, 0}, {}, &s), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::Scheduler::Create({WT_SCHED_MAX_WORKERS + 1, 60, 0}, {}, &s),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::Scheduler::Create({1, 0, 0}, {}, &s), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::Scheduler::Create({1, 241, 0}, {}, &s),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(s, nullptr);
}

TEST(SchedulerTest, DrawsTheFirstFrameThenWaitsToBeActivated) {
  Fixture f;
  auto a = f.Add(Frames(10, 20));
  ASSERT_TRUE(a->sink->WaitShown(1, milliseconds(1000)));
  Sleep(150);
  EXPECT_EQ(a->sink->shown(), std::vector<uint32_t>{0});
  EXPECT_EQ(f.Stats().frames_drawn, 1u);
  f.Remove(a.get());
}

// Timers on loaded machines (and hosted CI runners) wake late; the
// scheduler then skips frames to keep the content's time. So these tests
// check where playback is, not how many frames it showed.

TEST(SchedulerTest, PlaysAtTheContentRate) {
  Fixture f;
  auto a = f.Add(Frames(50, 25));  // 40 fps, 1.25 s.
  ASSERT_TRUE(a->sink->WaitShown(1, milliseconds(1000)));
  const auto start = std::chrono::steady_clock::now();
  f.sched->SetActive(a->player, true);
  Sleep(500);
  f.sched->SetActive(a->player, false);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const std::vector<uint32_t> shown = a->sink->shown();
  EXPECT_GE(shown.size(), 5u) << "it plays";
  EXPECT_LE(shown.size(), 22u) << "never faster than the content";
  // On the content clock: frame k is due k * 25 ms after starting.
  const double expect = static_cast<double>(elapsed / milliseconds(25));
  EXPECT_NEAR(static_cast<double>(shown.back()), expect, 4.0);
  // In order: each frame follows the last, or skips ahead to keep time.
  for (size_t i = 1; i < shown.size(); ++i) EXPECT_GT(shown[i], shown[i - 1]);
  EXPECT_EQ(a->sink->overlaps(), 0);
  EXPECT_EQ(a->log->exclusive.overlaps(), 0);
  EXPECT_EQ(a->sink->protocol_errors(), 0);
  f.Remove(a.get());
}

TEST(SchedulerTest, ContentFasterThanMaxFpsSkipsFrames) {
  Fixture f(/*workers=*/2, /*max_fps=*/20);
  auto a = f.Add(Frames(600, 10));  // 100 fps.
  ASSERT_TRUE(a->sink->WaitShown(1, milliseconds(1000)));
  const auto start = std::chrono::steady_clock::now();
  f.sched->SetActive(a->player, true);
  Sleep(1000);
  f.sched->SetActive(a->player, false);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const std::vector<uint32_t> shown = a->sink->shown();
  // At most 20 frames a second (plus the first), each about 5 frames on.
  EXPECT_LE(shown.size(), 23u);
  EXPECT_GE(shown.size(), 8u);
  EXPECT_GT(f.Stats().frames_skipped, 50u);
  // Content time kept: the last frame is about elapsed / 10 ms.
  const double expect = static_cast<double>(elapsed / milliseconds(10));
  EXPECT_NEAR(static_cast<double>(shown.back()), expect, 15.0);
  f.Remove(a.get());
}

// CPU time of the whole process so far.
Nanos ProcessCpuTime() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return std::chrono::seconds(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
         std::chrono::microseconds(u.ru_utime.tv_usec + u.ru_stime.tv_usec);
}

TEST(SchedulerTest, IdlesWithoutCpuWhenEverythingIsPaused) {
  Fixture f(/*workers=*/4);
  std::vector<std::unique_ptr<Fixture::Item>> items;
  for (int i = 0; i < 50; ++i) items.push_back(f.Add(Frames(30, 33)));
  for (auto& item : items) f.sched->SetActive(item->player, true);
  Sleep(300);
  EXPECT_GT(f.Stats().frames_drawn, 100u);
  for (auto& item : items) f.sched->SetActive(item->player, false);
  Sleep(100);  // Frames in flight land.
  const wt_sched_stats before = f.Stats();
  const Nanos cpu_before = ProcessCpuTime();
  Sleep(500);
  const wt_sched_stats after = f.Stats();
  const Nanos cpu = ProcessCpuTime() - cpu_before;
  EXPECT_EQ(after.wakeups, before.wakeups) << "the clock woke while idle";
  EXPECT_EQ(after.frames_drawn, before.frames_drawn);
  EXPECT_EQ(after.running, 0u);
  // The whole test process, sanitizers included: under 2% of a core.
  EXPECT_LT(cpu, milliseconds(10));
  for (auto& item : items) f.Remove(item.get());
}

TEST(SchedulerTest, MaxConcurrentPrefersPriorityThenRecency) {
  Fixture f(/*workers=*/2, /*max_fps=*/60, /*max_concurrent=*/2);
  std::vector<std::unique_ptr<Fixture::Item>> items;
  for (int i = 0; i < 4; ++i) items.push_back(f.Add(Frames(30, 20)));
  for (auto& item : items)
    ASSERT_TRUE(item->sink->WaitShown(1, milliseconds(1000)));
  const uint64_t priorities[] = {100, 50, 50, 10};
  for (int i = 0; i < 4; ++i) {
    f.sched->SetPriority(items[i]->player, priorities[i]);
    f.sched->SetActive(items[i]->player, true);
    Sleep(2);  // Distinct activation order: 0, 1, 2, 3.
  }
  Sleep(50);
  // 0 has the most priority; 2 ties with 1 but was activated later.
  EXPECT_TRUE(f.sched->IsRunning(items[0]->player));
  EXPECT_FALSE(f.sched->IsRunning(items[1]->player));
  EXPECT_TRUE(f.sched->IsRunning(items[2]->player));
  EXPECT_FALSE(f.sched->IsRunning(items[3]->player));
  EXPECT_EQ(f.Stats().running, 2u);
  EXPECT_EQ(f.Stats().active, 4u);
  const size_t frozen1 = items[1]->sink->shown_count();
  const size_t frozen3 = items[3]->sink->shown_count();
  const size_t playing0 = items[0]->sink->shown_count();
  const size_t playing2 = items[2]->sink->shown_count();
  Sleep(200);
  EXPECT_EQ(items[1]->sink->shown_count(), frozen1) << "frozen on its frame";
  EXPECT_EQ(items[3]->sink->shown_count(), frozen3);
  EXPECT_GE(items[0]->sink->shown_count(), playing0 + 2) << "plays";
  EXPECT_GE(items[2]->sink->shown_count(), playing2 + 2) << "plays";

  // A freed slot goes to the next in line.
  f.Remove(items[0].get());
  Sleep(50);
  EXPECT_TRUE(f.sched->IsRunning(items[1]->player));
  EXPECT_TRUE(f.sched->IsRunning(items[2]->player));
  EXPECT_FALSE(f.sched->IsRunning(items[3]->player));
  // A higher priority takes a slot.
  f.sched->SetPriority(items[3]->player, 1000);
  Sleep(50);
  EXPECT_TRUE(f.sched->IsRunning(items[3]->player));
  EXPECT_EQ(f.Stats().running, 2u);
  for (size_t i = 1; i < items.size(); ++i) f.Remove(items[i].get());
}

TEST(SchedulerTest, ASeekShowsTheFrameWhilePaused) {
  Fixture f;
  auto a = f.Add(Frames(20, 20));
  ASSERT_TRUE(a->sink->WaitShown(1, milliseconds(1000)));
  EXPECT_EQ(f.sched->Seek(a->player, 20), WT_ERR_INVALID_ARGUMENT);
  ASSERT_EQ(f.sched->Seek(a->player, 7), WT_OK);
  ASSERT_TRUE(a->sink->WaitShown(2, milliseconds(1000)));
  Sleep(100);
  EXPECT_EQ(a->sink->shown(), (std::vector<uint32_t>{0, 7}));
  f.Remove(a.get());
}

TEST(SchedulerTest, NonLoopingStopsOnTheLastFrameUntilASeek) {
  Fixture f;
  auto a = f.Add(Frames(5, 20), /*loop=*/false);
  f.sched->SetActive(a->player, true);
  const auto ends_on_last = [](const std::vector<uint32_t>& shown) {
    return !shown.empty() && shown.back() == 4u;
  };
  ASSERT_TRUE(a->sink->WaitUntil(ends_on_last, milliseconds(2000)));
  Sleep(150);
  std::vector<uint32_t> shown = a->sink->shown();
  EXPECT_EQ(shown.front(), 0u);
  EXPECT_EQ(shown.back(), 4u) << "held: nothing after the last frame";
  EXPECT_LE(shown.size(), 5u);
  for (size_t i = 1; i < shown.size(); ++i) EXPECT_GT(shown[i], shown[i - 1]);
  EXPECT_EQ(f.Stats().running, 0u) << "an ended player gives its slot up";

  const size_t before = shown.size();
  ASSERT_EQ(f.sched->Seek(a->player, 0), WT_OK);
  ASSERT_TRUE(a->sink->WaitUntil(
      [&](const std::vector<uint32_t>& v) {
        return v.size() > before + 1 && v.back() == 4u;
      },
      milliseconds(2000)));
  Sleep(100);
  shown = a->sink->shown();
  EXPECT_EQ(shown[before], 0u) << "the seek showed frame 0";
  EXPECT_EQ(shown.back(), 4u) << "and it played to the end again";
  EXPECT_LE(shown.size(), before + 5);
  f.Remove(a.get());
}

TEST(SchedulerTest, NoTargetWaitsForARedraw) {
  Fixture f;
  auto a = f.Add(Frames(10, 20), true, WT_NO_TARGET);
  f.sched->SetActive(a->player, true);
  Sleep(200);
  EXPECT_EQ(a->sink->acquires(), 1) << "parked after the first answer";
  EXPECT_EQ(a->sink->shown_count(), 0u);
  a->sink->set_answer(WT_OK);
  f.sched->Redraw(a->player);
  ASSERT_TRUE(a->sink->WaitShown(3, milliseconds(1000)));
  f.Remove(a.get());
}

TEST(SchedulerTest, BusyRetriesOnTheNextTick) {
  Fixture f;
  auto a = f.Add(Frames(10, 20), true, WT_OK, /*busy_for=*/3);
  ASSERT_TRUE(a->sink->WaitShown(1, milliseconds(1000)));
  EXPECT_EQ(a->sink->acquires(), 4);
  EXPECT_EQ(a->sink->shown(), std::vector<uint32_t>{0});
  f.Remove(a.get());
}

TEST(SchedulerTest, SinkErrorsBackOff) {
  Fixture f;
  auto a = f.Add(Frames(10, 20), true, WT_ERR_IO);
  Sleep(250);
  // Retried about every 100 ms, not every tick.
  EXPECT_GE(a->sink->acquires(), 2);
  EXPECT_LE(a->sink->acquires(), 4);
  a->sink->set_answer(WT_OK);
  ASSERT_TRUE(a->sink->WaitShown(1, milliseconds(1000)));
  f.Remove(a.get());
}

TEST(SchedulerTest, DestroyWaitsForTheFrameInFlight) {
  Fixture f;
  auto a = f.Add(Frames(10, 20));
  ASSERT_TRUE(a->sink->WaitShown(1, milliseconds(1000)));
  a->sink->Close();
  ASSERT_EQ(f.sched->Seek(a->player, 3), WT_OK);
  ASSERT_TRUE(a->sink->WaitBlocked(milliseconds(1000)));
  std::atomic<bool> returned{false};
  std::thread remover([&] {
    f.sched->RemovePlayer(a->player);
    returned = true;
    a->sink->MarkDestroyed();
  });
  Sleep(100);
  EXPECT_FALSE(returned.load());
  a->sink->Open();
  remover.join();
  EXPECT_TRUE(returned.load());
  EXPECT_EQ(a->sink->late_calls(), 0);
  EXPECT_EQ(f.Stats().players, 0u);
}

TEST(SchedulerTest, ARedrawDuringAFrameInFlightIsNotLost) {
  Fixture f;
  // Frame 1 shows for 2 s: without the redraw, frame 2 would wait for it.
  auto a = f.Add({50, 2000, 2000});
  ASSERT_TRUE(a->sink->WaitShown(1, milliseconds(1000)));
  a->sink->Close();
  f.sched->SetActive(a->player, true);
  ASSERT_TRUE(a->sink->WaitBlocked(milliseconds(1000)));  // Frame 1.
  f.sched->Redraw(a->player);  // A new target while frame 1 is in flight.
  a->sink->Open();
  ASSERT_TRUE(a->sink->WaitShown(3, milliseconds(500)))
      << "the new target waited for frame 1's duration";
  EXPECT_EQ(a->sink->shown(), (std::vector<uint32_t>{0, 1, 2}));
  f.Remove(a.get());
}

TEST(SchedulerTest, ARedrawAfterTheEndDrawsTheLastFrameAgain) {
  Fixture f;
  auto a = f.Add(Frames(2, 40), /*loop=*/false);
  f.sched->SetActive(a->player, true);
  ASSERT_TRUE(a->sink->WaitShown(2, milliseconds(1000)));
  Sleep(100);
  f.sched->Redraw(a->player);
  ASSERT_TRUE(a->sink->WaitShown(3, milliseconds(500)));
  EXPECT_EQ(a->sink->shown(), (std::vector<uint32_t>{0, 1, 1}));
  f.Remove(a.get());
}

TEST(SchedulerTest, ReportsTheFirstDecodeErrorOnly) {
  Fixture f;
  auto a = f.Add(Frames(4, 40));  // Slower than the clock: nothing skipped.
  a->log->broken_frame = 2;
  f.sched->SetActive(a->player, true);
  Sleep(700);  // About four loops over the broken frame.
  f.sched->SetActive(a->player, false);
  EXPECT_EQ(a->sink->errors(), 1);
  EXPECT_EQ(a->sink->first_error(), WT_ERR_DECODE);
  const std::vector<uint32_t> shown = a->sink->shown();
  EXPECT_EQ(std::count(shown.begin(), shown.end(), 2u), 0);
  EXPECT_GE(shown.size(), 6u) << "plays on past it";
  f.Remove(a.get());
}

TEST(SchedulerTest, SetOptionsResizesThePool) {
  Fixture f(/*workers=*/1);
  EXPECT_EQ(f.Stats().workers, 1u);
  EXPECT_EQ(f.sched->SetOptions({4, 30, 0}), WT_OK);
  EXPECT_EQ(f.Stats().workers, 4u);
  auto a = f.Add(Frames(10, 20));
  f.sched->SetActive(a->player, true);
  Sleep(50);
  EXPECT_EQ(f.sched->SetOptions({1, 60, 0}), WT_OK);
  EXPECT_EQ(f.Stats().workers, 1u);
  EXPECT_EQ(f.sched->SetOptions({0, 60, 0}), WT_ERR_INVALID_ARGUMENT);
  const size_t shown = a->sink->shown_count();
  Sleep(100);
  EXPECT_GT(a->sink->shown_count(), shown) << "still playing on one worker";
  f.Remove(a.get());
}

TEST(SchedulerTest, ASingleFrameNeverPlays) {
  Fixture f;
  auto a = f.Add(Frames(1, 20));
  f.sched->SetActive(a->player, true);
  ASSERT_TRUE(a->sink->WaitShown(1, milliseconds(1000)));
  Sleep(150);
  EXPECT_EQ(a->sink->shown_count(), 1u);
  EXPECT_EQ(f.Stats().running, 0u);
  f.Remove(a.get());
}

TEST(SchedulerTest, HooksRunOnEveryThread) {
  struct Counts {
    std::atomic<int> started{0};
    std::atomic<int> stopped{0};
  } counts;
  wt_sched_hooks hooks{};
  hooks.context = &counts;
  hooks.thread_start = [](void* c, const char* name) {
    EXPECT_EQ(std::string(name).rfind("wt-", 0), 0u);
    static_cast<Counts*>(c)->started++;
  };
  hooks.thread_stop = [](void* c) { static_cast<Counts*>(c)->stopped++; };
  {
    std::unique_ptr<wt::Scheduler> s;
    ASSERT_EQ(wt::Scheduler::Create({3, 60, 0}, hooks, &s), WT_OK);
    Sleep(50);
  }
  EXPECT_EQ(counts.started.load(), 4);  // The clock and three workers.
  EXPECT_EQ(counts.stopped.load(), 4);
}

// --- C ABI on a real animation
// ------------------------------------------------

TEST(SchedApiTest, ArgumentChecks) {
  wt_sched_options o;
  wt_sched_options_init_default(&o);
  EXPECT_GE(o.worker_count, 1u);
  EXPECT_LE(o.worker_count, 4u);
  EXPECT_EQ(o.max_fps, 60u);
  EXPECT_EQ(o.max_concurrent, 0u);
  wt_sched_options_init_default(nullptr);

  EXPECT_EQ(wt_sched_create(nullptr, nullptr, nullptr),
            WT_ERR_INVALID_ARGUMENT);
  wt_sched* sched = reinterpret_cast<wt_sched*>(1);
  wt_sched_options bad = o;
  bad.max_fps = 0;
  EXPECT_EQ(wt_sched_create(&bad, nullptr, &sched), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(sched, nullptr);
  ASSERT_EQ(wt_sched_create(nullptr, nullptr, &sched), WT_OK);
  EXPECT_EQ(wt_sched_set_options(sched, nullptr), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_sched_set_options(sched, &bad), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_sched_get_stats(sched, nullptr), WT_ERR_INVALID_ARGUMENT);
  wt_sched_stats stats;
  EXPECT_EQ(wt_sched_get_stats(nullptr, &stats), WT_ERR_INVALID_ARGUMENT);
  ASSERT_EQ(wt_sched_get_stats(sched, &stats), WT_OK);
  EXPECT_EQ(stats.players, 0u);
  EXPECT_EQ(stats.workers, o.worker_count);

  wt_test::Anim anim;
  ASSERT_EQ(wt_test::OpenAnim("vp9_opaque_256.webm", 32, 32, &anim), WT_OK);
  wt_sink sink{};
  wt_player* player = reinterpret_cast<wt_player*>(1);
  EXPECT_EQ(wt_player_create(sched, anim.p, true, &sink, &player),
            WT_ERR_INVALID_ARGUMENT);  // No acquire/release.
  EXPECT_EQ(player, nullptr);
  EXPECT_EQ(wt_player_create(nullptr, anim.p, true, &sink, &player),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_player_create(sched, nullptr, true, &sink, &player),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_player_create(sched, anim.p, true, nullptr, &player),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_player_seek(nullptr, 0), WT_ERR_INVALID_ARGUMENT);
  wt_player_destroy(nullptr);
  wt_player_set_active(nullptr, true);
  wt_player_set_priority(nullptr, 1);
  wt_player_redraw(nullptr);
  wt_sched_destroy(nullptr);
  wt_sched_destroy(sched);
}

// Collects every frame a real animation draws, by index.
struct PixelSink {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> buffer;
  std::mutex mu;
  std::map<uint32_t, std::vector<uint8_t>> frames;

  wt_sink sink() {
    wt_sink s{};
    s.context = this;
    s.acquire = [](void* c, wt_buffer* out) -> wt_status {
      auto* self = static_cast<PixelSink*>(c);
      out->pixels = self->buffer.data();
      out->size = self->buffer.size();
      out->stride = self->width * 4;
      out->format = WT_PIXEL_FORMAT_RGBA8888;
      return WT_OK;
    };
    s.release = [](void* c, wt_status status, const wt_frame_info* frame) {
      auto* self = static_cast<PixelSink*>(c);
      if (status != WT_OK) return;
      std::lock_guard<std::mutex> lk(self->mu);
      self->frames[frame->index] = self->buffer;
    };
    return s;
  }
};

TEST(SchedApiTest, PlaysARealAnimationFrameExactly) {
  constexpr uint32_t kSize = 48;
  wt_sched* sched = nullptr;
  ASSERT_EQ(wt_sched_create(nullptr, nullptr, &sched), WT_OK);
  wt_test::Anim anim;
  ASSERT_EQ(wt_test::OpenAnim("vp9_alpha_vfr.webm", kSize, kSize, &anim),
            WT_OK);
  PixelSink out;
  out.width = out.height = kSize;
  out.buffer.assign(size_t{kSize} * kSize * 4, 0);
  wt_sink sink = out.sink();
  wt_player* player = nullptr;
  ASSERT_EQ(wt_player_create(sched, anim.p, true, &sink, &player), WT_OK);
  anim.p = nullptr;  // The player owns it now.
  wt_player_set_priority(player, 1);
  wt_player_set_active(player, true);
  Sleep(800);
  wt_player_set_active(player, false);
  ASSERT_EQ(wt_player_seek(player, 3), WT_OK);
  Sleep(100);
  wt_sched_stats stats;
  ASSERT_EQ(wt_sched_get_stats(sched, &stats), WT_OK);
  EXPECT_EQ(stats.players, 1u);
  wt_player_destroy(player);
  wt_sched_destroy(sched);

  // Every frame drawn matches a direct decode of that frame.
  ASSERT_GT(out.frames.size(), 3u);
  ASSERT_EQ(out.frames.count(3), 1u);
  wt_test::Anim ref;
  ASSERT_EQ(wt_test::OpenAnim("vp9_alpha_vfr.webm", kSize, kSize, &ref), WT_OK);
  for (const auto& [index, pixels] : out.frames) {
    ASSERT_EQ(wt_anim_seek(ref.p, index), WT_OK);
    std::vector<uint8_t> expect;
    ASSERT_EQ(wt_test::Next(ref, &expect), WT_OK);
    EXPECT_EQ(pixels, expect) << "frame " << index;
  }
}

// --- Anim::SkipFrame
// ----------------------------------------------------------

TEST(SkipFrameTest, SkippingThenDrawingMatchesDrawingEveryFrame) {
  wt_test::Anim all, skipping;
  ASSERT_EQ(wt_test::OpenAnim("vp9_alpha_512.webm", 64, 64, &all), WT_OK);
  ASSERT_EQ(wt_test::OpenAnim("vp9_alpha_512.webm", 64, 64, &skipping), WT_OK);
  wt_info info;
  ASSERT_EQ(wt_anim_get_info(all.p, &info, nullptr, nullptr), WT_OK);
  std::vector<uint8_t> a, b;
  for (uint32_t i = 0; i < info.frame_count; ++i) {
    ASSERT_EQ(wt_test::Next(all, &a), WT_OK);
    if (i % 3 != 2) {
      wt_frame_info frame;
      ASSERT_EQ(skipping.p->anim->SkipFrame(&frame), WT_OK);
      EXPECT_EQ(frame.index, i);
      continue;
    }
    ASSERT_EQ(wt_test::Next(skipping, &b), WT_OK);
    EXPECT_EQ(a, b) << "frame " << i;
  }
  EXPECT_EQ(skipping.p->anim->SkipFrame(nullptr), WT_END);
}

TEST(SkipFrameTest, APassWithSkippedFramesStillWritesTheCache) {
  namespace fs = std::filesystem;
  const fs::path dir =
      fs::temp_directory_path() / ("wt_sched_skip_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string path = (dir / "skip.wsc").string();
  wt_test::Source source;
  ASSERT_EQ(wt_test::OpenSource("vp9_alpha_vfr.webm", &source), WT_OK);
  wt_test::Anim anim;
  ASSERT_EQ(wt_anim_create_cached(source.p, 32, 32, path.c_str(), &anim.p),
            WT_OK);
  wt_info info;
  ASSERT_EQ(wt_anim_get_info(anim.p, &info, nullptr, nullptr), WT_OK);
  std::vector<uint8_t> px;
  for (uint32_t i = 0; i < info.frame_count; ++i) {
    if (i % 2 == 0) {
      ASSERT_EQ(wt_test::Next(anim, &px), WT_OK);
    } else {
      ASSERT_EQ(anim.p->anim->SkipFrame(nullptr), WT_OK);
    }
  }
  EXPECT_EQ(anim.p->anim->cache_mode(), wt::Anim::CacheMode::kReading);
  EXPECT_TRUE(fs::exists(path));
  // Skips from the cache read nothing, and drawing still works.
  ASSERT_EQ(wt_anim_seek(anim.p, 0), WT_OK);
  ASSERT_EQ(anim.p->anim->SkipFrame(nullptr), WT_OK);
  wt_frame_info frame;
  ASSERT_EQ(wt_test::Next(anim, &px, WT_PIXEL_FORMAT_RGBA8888, &frame), WT_OK);
  EXPECT_EQ(frame.index, 1u);
  wt_anim_destroy(anim.p);
  anim.p = nullptr;
  fs::remove_all(dir);
}

}  // namespace
