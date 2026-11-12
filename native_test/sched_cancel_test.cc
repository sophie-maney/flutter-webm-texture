// T6.1, S-05 (ADR 0027): the scheduler decodes a frame before it takes the
// sink's buffer, and wt_player_destroy, wt_atlas_attach/detach and
// wt_sched_set_options cancel a frame in flight instead of waiting for a
// long catch-up decode.
//
// - AnimCancelTest: wt::Anim stops a catch-up between frames, moves nothing,
//   and resumes it later; a cache file completed by a cancelled call is
//   finished by the next one.
// - SchedCancelTest: removal, retargeting (atlas attach and detach) and a
//   smaller worker pool return within a few milliseconds while a decode that
//   would take seconds is in flight; a decoded frame waits for a buffer
//   without being decoded again; the sink is never held during a decode.
// - SchedCancelStressTest: cancellations racing completions, under churn,
//   retargets and a pool that keeps resizing. Every sink checks that no
//   frame is shown torn, nothing is called after its player was destroyed
//   or retargeted away, and every player still reaches the frame it was
//   last asked for. Built with -DWT_TSAN=ON it is the race check.
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "api/anim.h"
#include "api/cancel.h"
#include "atlas/compositor.h"
#include "demux/sniff.h"
#include "demux/source.h"
#include "gtest/gtest.h"
#include "sched/frame_source.h"
#include "sched/scheduler.h"
#include "sched_util.h"
#include "wt_api.h"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using wt::CancelFlag;
using wt::kCancelled;
using wt_test::Exclusive;
using wt_test::FakeSink;
using wt_test::FakeSource;
using Bytes = std::vector<uint8_t>;

constexpr auto kWait = milliseconds(5000);

template <typename Done>
bool WaitFor(Done done, milliseconds timeout = kWait) {
  const auto end = Clock::now() + timeout;
  while (Clock::now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(milliseconds(1));
  }
  return done();
}

template <typename F>
double MsToRun(F f) {
  const auto start = Clock::now();
  f();
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

// --- Anim -------------------------------------------------------------------

wt_limits DefaultLimits() {
  wt_limits limits;
  wt_limits_init_default(&limits);
  return limits;
}

struct TestAnim {
  std::unique_ptr<wt::ByteSource> source;
  std::unique_ptr<wt::Anim> anim;
};

wt_status MakeAnim(const std::string& name, uint32_t size,
                   const std::string& cache_path, TestAnim* out) {
  const std::string path = (fs::path(WT_TEST_ASSETS_DIR) / name).string();
  wt_status s =
      wt::ByteSource::OpenFile(path.c_str(), UINT64_MAX, &out->source);
  if (s != WT_OK) return s;
  const wt::ByteView bytes = out->source->whole();
  return wt::Anim::Create(bytes, wt::Sniff(bytes), DefaultLimits(), size, size,
                          cache_path.empty() ? nullptr : cache_path.c_str(),
                          &out->anim);
}

Bytes FrameAt(wt::Anim* anim, uint32_t index) {
  Bytes px(size_t{anim->width()} * anim->height() * 4);
  EXPECT_EQ(anim->Seek(index), WT_OK);
  EXPECT_EQ(anim->NextFrame(px.data(), px.size(), anim->width() * 4,
                            WT_PIXEL_FORMAT_RGBA8888, nullptr),
            WT_OK);
  return px;
}

// A catch-up cancelled before or during its run moves nothing; the next call
// carries on and draws exactly the frame an uninterrupted decode draws.
TEST(AnimCancelTest, ACancelledCatchUpMovesNothingAndResumes) {
  struct Case {
    const char* name;
    uint32_t target;  // Far from the frame before it that decoding starts at.
  };
  for (const Case c : {Case{"vp9_alpha_512.webm", 44}, Case{"anim.webp", 15},
                       Case{"vp8_alpha_256.webm", 29}}) {
    SCOPED_TRACE(c.name);
    TestAnim ref;
    ASSERT_EQ(MakeAnim(c.name, 64, "", &ref), WT_OK);
    const Bytes want = FrameAt(ref.anim.get(), c.target);

    TestAnim a;
    ASSERT_EQ(MakeAnim(c.name, 64, "", &a), WT_OK);
    wt::Anim* anim = a.anim.get();
    ASSERT_EQ(anim->Seek(c.target), WT_OK);
    CancelFlag cancel;
    cancel.Cancel();
    wt_frame_info info{};
    EXPECT_EQ(anim->PrepareFrame(&info, &cancel), kCancelled);
    EXPECT_EQ(anim->SkipFrame(&info, &cancel), kCancelled);
    // Cancelled from another thread while it catches up.
    for (int round = 0; round < 3; ++round) {
      CancelFlag later;
      std::thread canceller([&later, round] {
        std::this_thread::sleep_for(std::chrono::microseconds(200 * round));
        later.Cancel();
      });
      const wt_status s = anim->PrepareFrame(&info, &later);
      canceller.join();
      if (s == WT_OK) break;  // Finished first: as good.
      ASSERT_EQ(s, kCancelled);
    }
    // Whatever happened above, the next uncancelled call shows the target,
    // unless the race above already did.
    if (info.index != c.target) {
      ASSERT_EQ(anim->PrepareFrame(&info), WT_OK);
    }
    EXPECT_EQ(info.index, c.target);
    Bytes got(want.size());
    ASSERT_EQ(anim->WriteFrame(got.data(), got.size(), anim->width() * 4,
                               WT_PIXEL_FORMAT_RGBA8888),
              WT_OK);
    EXPECT_TRUE(got == want);
    // WriteFrame rewrites the same frame as often as asked.
    Bytes again(want.size());
    ASSERT_EQ(anim->WriteFrame(again.data(), again.size(), anim->width() * 4,
                               WT_PIXEL_FORMAT_RGBA8888),
              WT_OK);
    EXPECT_TRUE(again == want);
    EXPECT_EQ(anim->WriteFrame(again.data(), 3, anim->width() * 4,
                               WT_PIXEL_FORMAT_RGBA8888),
              WT_ERR_INVALID_ARGUMENT);
  }
}

// The fsync, rename and eviction of a finished cache file don't run in a
// cancelled call: the next call does them.
TEST(AnimCancelTest, ACancelledLastFrameFinishesTheFileOnTheNextCall) {
  const fs::path dir = fs::temp_directory_path() /
                       ("wt_cancel_cache_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string path = (dir / "c.wsc").string();
  TestAnim a;
  ASSERT_EQ(MakeAnim("vp8_alpha_256.webm", 32, path, &a), WT_OK);
  wt::Anim* anim = a.anim.get();
  const uint32_t frames = anim->info().frame_count;
  wt_frame_info info{};
  for (uint32_t i = 0; i + 1 < frames; ++i) {
    ASSERT_EQ(anim->PrepareFrame(&info), WT_OK);
  }
  CancelFlag cancel;
  cancel.Cancel();
  // The last frame needs no catch-up, so it is decoded; only the file waits.
  ASSERT_EQ(anim->PrepareFrame(&info, &cancel), WT_OK);
  EXPECT_EQ(info.index, frames - 1);
  EXPECT_EQ(anim->cache_mode(), wt::Anim::CacheMode::kWriting);
  EXPECT_FALSE(fs::exists(path));
  ASSERT_EQ(anim->Seek(0), WT_OK);
  // Still cancelled: nothing moves.
  EXPECT_EQ(anim->PrepareFrame(&info, &cancel), kCancelled);
  EXPECT_FALSE(fs::exists(path));
  ASSERT_EQ(anim->PrepareFrame(&info), WT_OK);
  EXPECT_EQ(info.index, 0u);
  EXPECT_EQ(anim->cache_mode(), wt::Anim::CacheMode::kReading);
  EXPECT_TRUE(fs::exists(path));
  a.anim.reset();
  fs::remove_all(dir);
}

// --- Scheduler
// ----------------------------------------------------------------

// A frame source whose every Prepare and Skip first "catches up" `steps`
// steps of `step_us` each, checking the cancel flag before every step, as
// wt::Anim's catch-up checks it before every frame. Progress survives a
// cancellation: the next call carries on. Write fills every pixel with the
// frame index, so a sink can tell a torn frame.
class SlowSource final : public wt::FrameSource {
 public:
  struct Control {
    Exclusive exclusive;
    std::atomic<uint32_t> steps{0};
    std::atomic<uint32_t> step_us{500};
    std::atomic<int> catching_up{0};      // Calls inside the loop now.
    std::atomic<uint64_t> steps_done{0};  // Over the source's life.
    std::atomic<uint32_t> cancels{0};     // Calls that returned kCancelled.
    std::atomic<uint32_t> prepares{0};    // Successful Prepares.
    std::atomic<uint32_t> last_written{UINT32_MAX};
  };

  SlowSource(std::vector<uint32_t> durations, uint32_t size,
             std::shared_ptr<Control> control)
      : durations_(std::move(durations)),
        size_(size),
        control_(std::move(control)) {}

  void GetOutputSize(uint32_t* w, uint32_t* h) const override {
    *w = size_;
    *h = size_;
  }
  uint32_t frame_count() const override {
    return static_cast<uint32_t>(durations_.size());
  }
  uint32_t frame_duration_ms(uint32_t index) const override {
    return durations_[index];
  }
  wt_status Seek(uint32_t index) override {
    Exclusive::Scope scope(&control_->exclusive);
    if (index >= frame_count()) return WT_ERR_INVALID_ARGUMENT;
    cursor_ = index;
    done_ = 0;  // A seek starts a new catch-up.
    return WT_OK;
  }
  wt_status Skip(const CancelFlag& cancel) override {
    Exclusive::Scope scope(&control_->exclusive);
    if (cursor_ >= frame_count()) return WT_END;
    if (!CatchUp(cancel)) return kCancelled;
    ++cursor_;
    return WT_OK;
  }
  wt_status Prepare(const CancelFlag& cancel, wt_frame_info* out) override {
    Exclusive::Scope scope(&control_->exclusive);
    std::memset(out, 0, sizeof(*out));
    if (cursor_ >= frame_count()) return WT_END;
    if (!CatchUp(cancel)) return kCancelled;
    prepared_ = cursor_++;
    control_->prepares.fetch_add(1);
    out->index = prepared_;
    out->duration_ms = durations_[prepared_];
    return WT_OK;
  }
  wt_status Write(const wt_buffer& dst) override {
    Exclusive::Scope scope(&control_->exclusive);
    const size_t row = size_t{size_} * 4;
    if (dst.pixels == nullptr || dst.stride < row ||
        dst.size < size_t{dst.stride} * (size_ - 1) + row) {
      return WT_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t y = 0; y < size_; ++y) {
      uint8_t* p = dst.pixels + size_t{y} * dst.stride;
      for (uint32_t x = 0; x < size_; ++x) {
        std::memcpy(p + size_t{x} * 4, &prepared_, 4);
      }
    }
    control_->last_written.store(prepared_);
    return WT_OK;
  }

 private:
  bool CatchUp(const CancelFlag& cancel) {
    control_->catching_up.fetch_add(1);
    bool finished = true;
    while (done_ < control_->steps.load()) {
      if (cancel.cancelled()) {
        control_->cancels.fetch_add(1);
        finished = false;
        break;
      }
      std::this_thread::sleep_for(
          std::chrono::microseconds(control_->step_us.load()));
      ++done_;
      control_->steps_done.fetch_add(1);
    }
    if (finished) done_ = 0;
    control_->catching_up.fetch_sub(1);
    return finished;
  }

  const std::vector<uint32_t> durations_;
  const uint32_t size_;
  const std::shared_ptr<Control> control_;
  uint32_t cursor_ = 0;
  uint32_t done_ = 0;  // Steps of the current catch-up already done.
  uint32_t prepared_ = 0;
};

// A wt_sink that checks every frame it is given whole: each 32-bit pixel
// must hold the frame index (SlowSource::Write), over a buffer acquire
// filled with garbage. Also checks the protocol, and counts calls made after
// Retire() (the player was destroyed or retargeted away).
class CheckingSink {
 public:
  explicit CheckingSink(uint32_t size) : size_(size) {}

  wt_sink sink() {
    wt_sink s{};
    s.context = this;
    s.acquire = &CheckingSink::Acquire;
    s.release = &CheckingSink::Release;
    s.error = &CheckingSink::Error;
    return s;
  }

  void set_answer(wt_status s) { answer_.store(s); }
  void Retire() { retired_.store(true); }

  int acquires() const { return acquires_.load(); }
  int late_calls() const { return late_.load(); }
  int torn() const { return torn_.load(); }
  int protocol_errors() const { return protocol_.load(); }
  int overlaps() const { return exclusive_.overlaps(); }
  uint32_t last_shown() const { return last_shown_.load(); }
  int shown_count() const { return shown_.load(); }

 private:
  static wt_status Acquire(void* context, wt_buffer* out) {
    auto* self = static_cast<CheckingSink*>(context);
    if (self->retired_.load()) self->late_.fetch_add(1);
    Exclusive::Scope scope(&self->exclusive_);
    self->acquires_.fetch_add(1);
    const wt_status answer = self->answer_.load();
    if (answer != WT_OK) return answer;
    if (self->in_attempt_) self->protocol_.fetch_add(1);
    self->in_attempt_ = true;
    self->thread_ = std::this_thread::get_id();
    self->pixels_.assign(size_t{self->size_} * self->size_ * 4, 0xEE);
    out->pixels = self->pixels_.data();
    out->size = self->pixels_.size();
    out->stride = self->size_ * 4;
    out->format = WT_PIXEL_FORMAT_RGBA8888;
    return WT_OK;
  }

  static void Release(void* context, wt_status status,
                      const wt_frame_info* frame) {
    auto* self = static_cast<CheckingSink*>(context);
    if (self->retired_.load()) self->late_.fetch_add(1);
    Exclusive::Scope scope(&self->exclusive_);
    if (!self->in_attempt_ || self->thread_ != std::this_thread::get_id()) {
      self->protocol_.fetch_add(1);
    }
    self->in_attempt_ = false;
    if (status != WT_OK) return;
    for (size_t i = 0; i < self->pixels_.size(); i += 4) {
      uint32_t v;
      std::memcpy(&v, self->pixels_.data() + i, 4);
      if (v != frame->index) {
        self->torn_.fetch_add(1);
        break;
      }
    }
    self->last_shown_.store(frame->index);
    self->shown_.fetch_add(1);
  }

  static void Error(void* context, wt_status) {
    auto* self = static_cast<CheckingSink*>(context);
    if (self->retired_.load()) self->late_.fetch_add(1);
  }

  const uint32_t size_;
  Exclusive exclusive_;
  std::atomic<wt_status> answer_{WT_OK};
  std::atomic<bool> retired_{false};
  std::atomic<int> acquires_{0};
  std::atomic<int> late_{0};
  std::atomic<int> torn_{0};
  std::atomic<int> protocol_{0};
  std::atomic<uint32_t> last_shown_{UINT32_MAX};
  std::atomic<int> shown_{0};
  // Used by the one worker in an attempt.
  bool in_attempt_ = false;
  std::thread::id thread_;
  Bytes pixels_;
};

// Long enough that waiting for it would be obvious: 20,000 steps of 0.5 ms
// (ten seconds).
constexpr uint32_t kLongCatchUp = 20000;
// What "doesn't wait" means. The bound is on the fastest of a few trials so
// that a loaded test machine can't make it flaky; every trial must still be
// far below the catch-up.
constexpr double kBoundMs = 20;
constexpr double kEveryTrialMs = 500;
constexpr int kTrials = 3;

std::unique_ptr<wt::Scheduler> MakeScheduler(uint32_t workers) {
  std::unique_ptr<wt::Scheduler> s;
  EXPECT_EQ(wt::Scheduler::Create({workers, 60, 0}, wt_sched_hooks{}, &s),
            WT_OK);
  return s;
}

wt::Player* AddSlow(wt::Scheduler* sched,
                    const std::shared_ptr<SlowSource::Control>& control,
                    const wt_sink& sink, uint32_t frames = 10) {
  std::unique_ptr<wt::FrameSource> src = std::make_unique<SlowSource>(
      std::vector<uint32_t>(frames, 40), 8, control);
  wt::Player* p = nullptr;
  EXPECT_EQ(sched->AddPlayer(&src, true, sink, &p), WT_OK);
  return p;
}

// Waits until a decode is well into its catch-up.
bool WaitCatchingUp(const SlowSource::Control& c) {
  const uint64_t base = c.steps_done.load();
  return WaitFor([&] {
    return c.catching_up.load() > 0 && c.steps_done.load() >= base + 4;
  });
}

void ExpectFast(const std::vector<double>& times) {
  ASSERT_FALSE(times.empty());
  EXPECT_LT(*std::min_element(times.begin(), times.end()), kBoundMs);
  for (double t : times) EXPECT_LT(t, kEveryTrialMs);
  std::printf("fastest %.2f ms, slowest %.2f ms\n",
              *std::min_element(times.begin(), times.end()),
              *std::max_element(times.begin(), times.end()));
}

TEST(SchedCancelTest, DestroyDoesntWaitForACatchUp) {
  auto sched = MakeScheduler(2);
  std::vector<double> times;
  for (int trial = 0; trial < kTrials; ++trial) {
    auto control = std::make_shared<SlowSource::Control>();
    control->steps = kLongCatchUp;
    CheckingSink sink(8);
    wt::Player* p = AddSlow(sched.get(), control, sink.sink());
    ASSERT_TRUE(WaitCatchingUp(*control));  // Frame 0's decode.
    times.push_back(MsToRun([&] { sched->RemovePlayer(p); }));
    sink.Retire();
    EXPECT_EQ(control->cancels.load(), 1u);
    EXPECT_EQ(control->catching_up.load(), 0);
    // The frame never got to the sink.
    EXPECT_EQ(sink.acquires(), 0);
  }
  std::this_thread::sleep_for(milliseconds(20));
  ExpectFast(times);
  EXPECT_EQ(sched->Stats().players, 0u);
}

TEST(SchedCancelTest, TheSinkIsntHeldDuringADecode) {
  auto sched = MakeScheduler(1);
  auto control = std::make_shared<SlowSource::Control>();
  control->steps = kLongCatchUp;
  CheckingSink sink(8);
  wt::Player* p = AddSlow(sched.get(), control, sink.sink());
  ASSERT_TRUE(WaitCatchingUp(*control));
  // Decoding, and no buffer taken: on Android, SetWindow(nullptr) would get
  // the window lock at once.
  EXPECT_EQ(sink.acquires(), 0);
  control->steps = 0;  // The decode finishes; then the frame is written.
  ASSERT_TRUE(WaitFor([&] { return sink.shown_count() == 1; }));
  EXPECT_EQ(sink.last_shown(), 0u);
  EXPECT_EQ(sink.acquires(), 1);
  EXPECT_EQ(sink.torn(), 0);
  sched->RemovePlayer(p);
  sink.Retire();
}

// A frame decoded when the sink had no buffer is written at the next
// attempt, not decoded again.
TEST(SchedCancelTest, ADecodedFrameWaitsForABuffer) {
  auto sched = MakeScheduler(2);
  auto log = std::make_shared<FakeSource::Log>();
  FakeSink sink;
  sink.set_busy_for(3);
  std::unique_ptr<wt::FrameSource> src =
      std::make_unique<FakeSource>(std::vector<uint32_t>(4, 40), log);
  wt::Player* p = nullptr;
  ASSERT_EQ(sched->AddPlayer(&src, true, sink.sink(), &p), WT_OK);
  ASSERT_TRUE(sink.WaitShown(1, kWait));
  EXPECT_EQ(sink.shown(), std::vector<uint32_t>{0});
  EXPECT_GE(sink.acquires(), 4);
  EXPECT_EQ(log->draws.load(), 1u) << "frame 0 was decoded again";
  // Playing on from there decodes each next frame once.
  sched->SetActive(p, true);
  ASSERT_TRUE(sink.WaitShown(3, kWait));
  sched->SetActive(p, false);
  sched->RemovePlayer(p);
  sink.MarkDestroyed();
  const std::vector<uint32_t> shown = sink.shown();
  EXPECT_EQ(shown[1], 1u);
  EXPECT_EQ(shown[2], 2u);
  EXPECT_EQ(sink.protocol_errors(), 0);
  EXPECT_EQ(log->exclusive.overlaps(), 0);
}

TEST(SchedCancelTest, SetOptionsDoesntWaitForACatchUp) {
  std::vector<double> times;
  for (int trial = 0; trial < kTrials; ++trial) {
    auto sched = MakeScheduler(2);
    // Two players busy on the two workers: shrinking to one worker must not
    // wait for the decode on the one that goes away.
    auto a = std::make_shared<SlowSource::Control>();
    auto b = std::make_shared<SlowSource::Control>();
    a->steps = kLongCatchUp;
    b->steps = kLongCatchUp;
    CheckingSink sa(8), sb(8);
    wt::Player* pa = AddSlow(sched.get(), a, sa.sink());
    wt::Player* pb = AddSlow(sched.get(), b, sb.sink());
    ASSERT_TRUE(WaitCatchingUp(*a));
    ASSERT_TRUE(WaitCatchingUp(*b));
    times.push_back(
        MsToRun([&] { EXPECT_EQ(sched->SetOptions({1, 60, 0}), WT_OK); }));
    EXPECT_EQ(sched->Stats().workers, 1u);
    EXPECT_EQ(a->cancels.load() + b->cancels.load(), 1u);
    // The cancelled frame is drawn by the worker that stays, and nothing
    // drawn so far was torn.
    a->steps = 0;
    b->steps = 0;
    ASSERT_TRUE(WaitFor(
        [&] { return sa.shown_count() >= 1 && sb.shown_count() >= 1; }));
    EXPECT_EQ(sa.last_shown(), 0u);
    EXPECT_EQ(sb.last_shown(), 0u);
    EXPECT_EQ(sa.torn() + sb.torn(), 0);
    sched->RemovePlayer(pa);
    sched->RemovePlayer(pb);
  }
  ExpectFast(times);
}

TEST(SchedCancelTest, AtlasAttachAndDetachDontWaitForACatchUp) {
  auto sched = MakeScheduler(2);
  wt_atlas_options options;
  wt::Atlas::DefaultOptions(&options);
  options.width = 256;
  options.max_height = 256;
  std::unique_ptr<wt::Atlas> atlas;
  ASSERT_EQ(wt::Atlas::Create(sched.get(), options, &atlas), WT_OK);

  std::vector<double> detach, attach;
  for (int trial = 0; trial < kTrials; ++trial) {
    auto control = std::make_shared<SlowSource::Control>();
    CheckingSink home(8);
    wt::Player* p = AddSlow(sched.get(), control, home.sink());
    ASSERT_TRUE(WaitFor([&] { return home.shown_count() == 1; }));
    wt_atlas_slot slot;
    const uint32_t prepared = control->prepares.load();
    ASSERT_EQ(atlas->Attach(p, &slot), WT_OK);
    // The slot's first frame.
    ASSERT_TRUE(WaitFor([&] {
      return control->prepares.load() > prepared &&
             control->last_written.load() == 0;
    }));

    // Detach while the slot's next frame is a long catch-up.
    control->steps = kLongCatchUp;
    ASSERT_EQ(sched->Seek(p, 5), WT_OK);
    ASSERT_TRUE(WaitCatchingUp(*control));
    detach.push_back(MsToRun([&] { EXPECT_EQ(atlas->Detach(p), WT_OK); }));
    // The home sink gets that frame once the decode is allowed to finish.
    control->steps = 0;
    ASSERT_TRUE(WaitFor([&] { return home.last_shown() == 5; }));

    // And attach while the home sink's next frame is a long catch-up.
    control->steps = kLongCatchUp;
    ASSERT_EQ(sched->Seek(p, 7), WT_OK);
    ASSERT_TRUE(WaitCatchingUp(*control));
    const int home_shown = home.shown_count();
    attach.push_back(
        MsToRun([&] { EXPECT_EQ(atlas->Attach(p, &slot), WT_OK); }));
    control->steps = 0;
    ASSERT_TRUE(WaitFor([&] { return control->last_written.load() == 7; }));
    EXPECT_EQ(home.shown_count(), home_shown) << "frame 7 went to the old sink";
    EXPECT_GE(control->cancels.load(), 2u);
    EXPECT_EQ(home.torn(), 0);
    EXPECT_EQ(home.protocol_errors(), 0);
    ASSERT_EQ(atlas->Detach(p), WT_OK);
    sched->RemovePlayer(p);
    home.Retire();
    EXPECT_EQ(control->exclusive.overlaps(), 0);
  }
  ExpectFast(detach);
  ExpectFast(attach);
}

// --- Stress -----------------------------------------------------------------

struct Live {
  wt::Player* player = nullptr;
  uint32_t frames = 0;
  std::shared_ptr<SlowSource::Control> control;
  // Every sink the player ever had; the last one is current.
  std::vector<std::unique_ptr<CheckingSink>> sinks;
};

TEST(SchedCancelStressTest, CancellationsRaceCompletions) {
  constexpr int kDrivers = 6;
  constexpr int kMaxAlive = 12;
  constexpr auto kRunFor = milliseconds(3000);
  constexpr uint32_t kSize = 4;
  auto sched = MakeScheduler(4);
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};
  std::atomic<int> created{0};
  std::atomic<int> retargets{0};
  std::mutex dead_mu;
  std::vector<Live> dead;

  auto create = [&](std::mt19937* rng) {
    Live live;
    live.frames = 2 + (*rng)() % 30;
    std::vector<uint32_t> durations(live.frames);
    for (uint32_t& d : durations) d = 5 + (*rng)() % 35;
    live.control = std::make_shared<SlowSource::Control>();
    live.control->steps = (*rng)() % 5;
    live.control->step_us = 100 + (*rng)() % 400;
    live.sinks.push_back(std::make_unique<CheckingSink>(kSize));
    std::unique_ptr<wt::FrameSource> src =
        std::make_unique<SlowSource>(std::move(durations), kSize, live.control);
    if (sched->AddPlayer(&src, (*rng)() % 4 != 0, live.sinks.back()->sink(),
                         &live.player) != WT_OK) {
      failures++;
    }
    created++;
    return live;
  };
  auto destroy = [&](Live live) {
    sched->RemovePlayer(live.player);
    for (auto& s : live.sinks) s->Retire();
    std::lock_guard<std::mutex> lk(dead_mu);
    dead.push_back(std::move(live));
  };

  auto drive = [&](int seed) {
    std::mt19937 rng(static_cast<uint32_t>(seed));
    std::vector<Live> alive;
    const auto start = Clock::now();
    while (Clock::now() - start < kRunFor) {
      const int op = static_cast<int>(rng() % 100);
      if (alive.size() < kMaxAlive && (alive.empty() || op < 15)) {
        alive.push_back(create(&rng));
        continue;
      }
      const size_t i = rng() % alive.size();
      Live& live = alive[i];
      if (op < 30) {
        sched->SetActive(live.player, rng() % 3 != 0);
      } else if (op < 45) {
        if (sched->Seek(live.player, rng() % live.frames) != WT_OK) {
          failures++;
        }
      } else if (op < 55) {
        sched->Redraw(live.player);
      } else if (op < 70) {
        // A new target: the old one is never called again.
        live.sinks.push_back(std::make_unique<CheckingSink>(kSize));
        wt_sink previous{};
        if (sched->Retarget(live.player, live.sinks.back()->sink(), nullptr,
                            nullptr, &previous) != WT_OK) {
          failures++;
        }
        live.sinks[live.sinks.size() - 2]->Retire();
        if (previous.context != live.sinks[live.sinks.size() - 2].get()) {
          failures++;
        }
        retargets++;
      } else if (op < 78) {
        const int mode = static_cast<int>(rng() % 3);
        live.sinks.back()->set_answer(mode == 0   ? WT_BUSY
                                      : mode == 1 ? WT_NO_TARGET
                                                  : WT_OK);
      } else if (op < 86) {
        live.control->steps = rng() % 8;
      } else if (op < 94) {
        destroy(std::move(live));
        alive.erase(alive.begin() + static_cast<std::ptrdiff_t>(i));
      }
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 1500));
    }
    // Every survivor still reaches the frame it is asked for: no cancelled
    // job left it stuck.
    for (Live& live : alive) {
      live.control->steps = 0;
      live.sinks.back()->set_answer(WT_OK);
      const uint32_t target = static_cast<uint32_t>(rng() % live.frames);
      sched->SetActive(live.player, false);
      if (sched->Seek(live.player, target) != WT_OK) failures++;
      sched->Redraw(live.player);
      CheckingSink* sink = live.sinks.back().get();
      if (!WaitFor([&] { return sink->last_shown() == target; })) {
        std::printf("stuck: wanted %u, shows %u\n", target, sink->last_shown());
        failures++;
      }
    }
    for (Live& live : alive) destroy(std::move(live));
  };

  std::thread tuner([&] {
    std::mt19937 rng(99);
    while (!done.load()) {
      const uint32_t workers = 1 + static_cast<uint32_t>(rng() % 4);
      if (sched->SetOptions({workers, 120, rng() % 3 == 0 ? 4u : 0u}) !=
          WT_OK) {
        failures++;
      }
      std::this_thread::sleep_for(milliseconds(rng() % 30));
    }
  });
  std::vector<std::thread> drivers;
  for (int i = 0; i < kDrivers; ++i) drivers.emplace_back(drive, i + 7);
  for (std::thread& t : drivers) t.join();
  done = true;
  tuner.join();
  const wt_sched_stats stats = sched->Stats();
  sched.reset();

  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(stats.players, 0u);
  uint32_t cancels = 0;
  int shown = 0;
  for (const Live& live : dead) {
    EXPECT_EQ(live.control->exclusive.overlaps(), 0);
    cancels += live.control->cancels.load();
    for (const auto& s : live.sinks) {
      EXPECT_EQ(s->late_calls(), 0);
      EXPECT_EQ(s->torn(), 0);
      EXPECT_EQ(s->protocol_errors(), 0);
      EXPECT_EQ(s->overlaps(), 0);
      shown += s->shown_count();
    }
  }
  EXPECT_GT(cancels, 0u) << "nothing was cancelled: the test proves nothing";
  EXPECT_GT(shown, 500);
  std::printf(
      "cancel stress: %d players, %d retargets, %u cancelled decodes, %d "
      "frames shown\n",
      created.load(), retargets.load(), cancels, shown);
}

}  // namespace
