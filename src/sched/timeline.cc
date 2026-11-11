#include "sched/timeline.h"

#include <algorithm>
#include <utility>

namespace wt {

namespace {

std::vector<Nanos> Clamped(std::vector<Nanos> durations) {
  for (Nanos& d : durations) d = std::max(d, kMinFrameTime);
  return durations;
}

Nanos Shortest(const std::vector<Nanos>& durations) {
  Nanos m = kMinFrameTime;
  if (!durations.empty()) {
    m = *std::min_element(durations.begin(), durations.end());
  }
  return m;
}

}  // namespace

Timeline::Timeline(std::vector<Nanos> durations, bool loop)
    : durations_(Clamped(std::move(durations))),
      loop_(loop),
      min_duration_(Shortest(durations_)) {
  remaining_ = durations_.empty() ? kMinFrameTime : durations_[0];
}

bool Timeline::NextAfter(uint32_t frame, uint32_t* next) const {
  if (frame + 1 < frame_count()) {
    *next = frame + 1;
    return true;
  }
  if (!loop_) return false;
  *next = 0;
  return true;
}

void Timeline::Seek(uint32_t frame) {
  shown_ = frame;
  dirty_ = true;
  ended_ = false;
  redraw_owed_ = false;
  ++generation_;
}

void Timeline::Redraw(TimePoint now) {
  uint32_t next;
  // Only when there is a next frame: a non-looping timeline on its last
  // frame (ended or about to be) must draw that frame again.
  if (running_ && animates() && !dirty_ && NextAfter(shown_, &next)) {
    // The next frame is as good as the lost one, and needs no seek. Stop
    // turns this back into a request if it stops before that frame is drawn.
    due_ = std::min(due_, now);
    redraw_owed_ = true;
    return;
  }
  dirty_ = true;
  redraw_owed_ = false;
  ++generation_;
}

void Timeline::Start(TimePoint now) {
  if (running_) return;
  running_ = true;
  due_ = now + remaining_;
}

void Timeline::Stop(TimePoint now) {
  if (!running_) return;
  running_ = false;
  remaining_ = std::clamp<Nanos>(due_ - now, Nanos::zero(), durations_[shown_]);
  if (redraw_owed_) {
    // A redraw was waiting for the next playback frame, which now won't
    // come: the new target would stay empty. Draw shown() instead.
    redraw_owed_ = false;
    dirty_ = true;
    ++generation_;
  }
}

void Timeline::Position(FrameJob* job) const {
  job->seek = true;
  job->from = cursor_;
  job->skips = 0;
  if (!cursor_known_) return;
  const uint32_t target = job->target;
  if (job->kind == FrameJob::Kind::kRequest) {
    // Redrawing the frame just shown means going back: always a seek.
    job->seek = cursor_ != target;
    return;
  }
  uint32_t distance;
  if (target >= cursor_) {
    distance = target - cursor_;
  } else if (loop_) {
    distance = frame_count() - cursor_ + target;  // Through the loop point.
  } else {
    return;
  }
  if (distance > kMaxSkips) return;
  job->seek = false;
  job->skips = distance;
}

FrameJob Timeline::PlanRequest() const {
  FrameJob job;
  job.kind = FrameJob::Kind::kRequest;
  job.target = shown_;
  job.generation = generation_;
  Position(&job);
  return job;
}

bool Timeline::PlanPlayback(TimePoint now, Nanos early, Nanos stall,
                            FrameJob* job) {
  if (!running_ || dirty_ || !animates()) return false;
  uint32_t target;
  if (!NextAfter(shown_, &target)) {
    ended_ = true;
    return false;
  }
  if (due_ > now + early) return false;
  TimePoint due = due_;
  if (now - due > stall) {
    // Far behind: carry on from the next frame as if it were on time.
    due = now;
  } else {
    // Skip every frame whose successor is already due too. Never lap the
    // animation in one job.
    for (uint32_t skipped = 0; skipped + 1 < frame_count(); ++skipped) {
      uint32_t after;
      if (!NextAfter(target, &after)) break;
      const TimePoint after_due = due + durations_[target];
      if (after_due > now + early) break;
      target = after;
      due = after_due;
    }
  }
  job->kind = FrameJob::Kind::kPlayback;
  job->target = target;
  job->due = due;
  job->generation = generation_;
  Position(job);
  return true;
}

void Timeline::Complete(const FrameJob& job, const FrameOutcome& outcome,
                        TimePoint done) {
  cursor_ = outcome.cursor;
  cursor_known_ = outcome.cursor_known;
  // A request made while the job ran wins over it.
  if (job.generation != generation_ || !outcome.drawn) return;
  shown_ = job.target;
  redraw_owed_ = false;  // A frame reached the (possibly new) target.
  const Nanos duration = durations_[shown_];
  if (job.kind == FrameJob::Kind::kRequest) {
    dirty_ = false;
    if (running_) {
      due_ = done + duration;
    } else {
      remaining_ = duration;
    }
    return;
  }
  // Stay on the content clock, measured from when the frame was due.
  due_ = job.due + duration;
  if (!running_) {
    remaining_ = std::clamp<Nanos>(due_ - done, Nanos::zero(), duration);
  }
}

}  // namespace wt
