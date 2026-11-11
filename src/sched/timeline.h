// wt::Timeline: the playback clock of one scheduled animation (T3.3).
//
// It decides which frame should be on screen and when, and how a worker gets
// the core's cursor there; it never touches pixels or threads. The scheduler
// (sched/scheduler.h) owns one per player and calls it under its mutex, with
// the time passed in, so the timing rules can be tested deterministically.
//
// Rules (ADR 0017):
// - Requests (the first frame, a seek, a redraw) come before playback: while
//   one is pending, nothing plays. A request draws `shown()` and costs a seek
//   unless the cursor is already there.
// - Playback follows the content's clock: the frame after shown() is due
//   when shown()'s duration has run out. A round that finds it due also skips
//   over every later frame that is already due, so content faster than the
//   clock (or a frame that came back late) keeps its timing; skipped frames
//   are still decoded but never drawn. More than `stall` late (the app was
//   suspended, a worker stalled), playback instead carries on from the next
//   frame as if it were on time, rather than racing to catch up.
// - A stopped timeline remembers how long shown() still had to stay, and
//   gives it that time when started again.
// - A non-looping timeline ends on its last frame; a seek lets it play again.
// - Each request bumps generation(): a job planned before it can't overwrite
//   what it asked for.
#ifndef WT_SCHED_TIMELINE_H_
#define WT_SCHED_TIMELINE_H_

#include <chrono>
#include <cstdint>
#include <vector>

namespace wt {

using SchedClock = std::chrono::steady_clock;
using TimePoint = SchedClock::time_point;
using Nanos = std::chrono::nanoseconds;

// Frames shorter than this (including 0 ms) are shown for this long, so a
// broken duration can't spin the scheduler.
constexpr Nanos kMinFrameTime = std::chrono::milliseconds(10);

// Playback reaches its target by skipping at most this many frames; further
// away, it seeks (which breaks a cache-writing pass but costs less).
constexpr uint32_t kMaxSkips = 16;

// One unit of work for a worker: bring the core's cursor to `target`, then
// draw it.
struct FrameJob {
  enum class Kind : uint8_t {
    kRequest,   // Draws shown(): the first frame, a seek or a redraw.
    kPlayback,  // The next frame of playback.
  };
  Kind kind = Kind::kRequest;
  uint32_t target = 0;
  // How to get the cursor to target: Seek(target), or else `skips` calls to
  // SkipFrame starting at `from`, seeking to frame 0 whenever the cursor is
  // past the last frame (a loop).
  bool seek = true;
  uint32_t from = 0;
  uint32_t skips = 0;
  // kPlayback: when target was due on the content clock.
  TimePoint due{};
  // Timeline::generation() when the job was planned.
  uint64_t generation = 0;
};

// What came of a FrameJob.
struct FrameOutcome {
  // The target used up its turn: it was drawn, or it failed to decode (the
  // previous frame stays on screen and playback moves on). False when the
  // sink had no buffer, so nothing was drawn.
  bool drawn = false;
  // Where the core's cursor ended up (the next frame it returns), if known.
  uint32_t cursor = 0;
  bool cursor_known = false;
};

class Timeline {
 public:
  // durations: one per frame, at least one; each is raised to kMinFrameTime.
  // Starts stopped, with a request to draw frame 0 and the cursor unknown.
  Timeline(std::vector<Nanos> durations, bool loop);

  uint32_t frame_count() const {
    return static_cast<uint32_t>(durations_.size());
  }
  // The shortest frame (after raising to kMinFrameTime).
  Nanos min_duration() const { return min_duration_; }
  bool loop() const { return loop_; }
  // The frame on screen, or the one a pending request will draw.
  uint32_t shown() const { return shown_; }
  // A request waits to be drawn.
  bool dirty() const { return dirty_; }
  bool running() const { return running_; }
  // A non-looping timeline passed its last frame (see PlanPlayback).
  bool ended() const { return ended_; }
  // Can move on by itself: more than one frame and not ended.
  bool animates() const { return durations_.size() > 1 && !ended_; }
  uint64_t generation() const { return generation_; }
  // While running: when the frame after shown() is due.
  TimePoint due() const { return due_; }

  // Requests frame `frame` (< frame_count): shown() becomes it, and a
  // non-looping timeline that ended may play again.
  void Seek(uint32_t frame);
  // The frame on screen was lost (a new surface). A running timeline with a
  // frame after shown() just makes that frame due now; otherwise shown() is
  // requested again. If the timeline stops before that next frame is drawn,
  // Stop requests shown() after all.
  void Redraw(TimePoint now);

  void Start(TimePoint now);
  void Stop(TimePoint now);

  // The job for the pending request. Requires dirty().
  FrameJob PlanRequest() const;

  // Plans the playback frame due by `now + early`: returns false when none
  // is (stopped, a request pending, nothing to animate, or not due yet).
  // `stall`: how late a frame may be before playback stops catching up.
  // Sets ended() when a non-looping timeline has nothing after shown().
  bool PlanPlayback(TimePoint now, Nanos early, Nanos stall, FrameJob* job);

  // Applies the outcome of `job`, which finished at `done`.
  void Complete(const FrameJob& job, const FrameOutcome& outcome,
                TimePoint done);

 private:
  // The frame after `frame`, wrapping when looping. False at the end of a
  // non-looping timeline.
  bool NextAfter(uint32_t frame, uint32_t* next) const;
  // Fills job's seek/from/skips to bring the cursor to job->target.
  void Position(FrameJob* job) const;

  const std::vector<Nanos> durations_;
  const bool loop_;
  const Nanos min_duration_;

  uint32_t shown_ = 0;
  bool dirty_ = true;
  uint64_t generation_ = 0;
  bool ended_ = false;
  bool running_ = false;
  TimePoint due_{};    // Meaningful while running_.
  Nanos remaining_{};  // Meaningful while stopped: shown_'s time left.
  uint32_t cursor_ = 0;
  bool cursor_known_ = false;
  // Redraw is waiting for the next playback frame (see Stop).
  bool redraw_owed_ = false;
};

}  // namespace wt

#endif  // WT_SCHED_TIMELINE_H_
