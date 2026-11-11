#include "sched/scheduler.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <thread>
#include <utility>

namespace wt {

namespace {

// After a sink error (not WT_BUSY or WT_NO_TARGET), wait this long before
// asking it again.
constexpr Nanos kErrorRetry = std::chrono::milliseconds(100);

// Playback this late or later carries on from the next frame instead of
// skipping ahead (at least two rounds).
constexpr Nanos kMinStall = std::chrono::milliseconds(100);

// A round may start this much before the previous one is a period old, to
// absorb timer jitter.
constexpr Nanos kRoundSlack = std::chrono::milliseconds(1);

constexpr const char* kWorkerNames[WT_SCHED_MAX_WORKERS] = {
    "wt-worker-0", "wt-worker-1", "wt-worker-2", "wt-worker-3",
    "wt-worker-4", "wt-worker-5", "wt-worker-6", "wt-worker-7",
};

void NameThread(const char* name) {
#if defined(__APPLE__)
  pthread_setname_np(name);
#else
  pthread_setname_np(pthread_self(), name);
#endif
}

Nanos PeriodFor(uint32_t max_fps) {
  return Nanos(std::chrono::seconds(1)) / max_fps;
}

// Dispatch order: the players that run first, then by priority and recency.
bool Better(const Player* a, uint64_t a_priority, uint64_t a_activated,
            const Player* b, uint64_t b_priority, uint64_t b_activated) {
  if (a_priority != b_priority) return a_priority > b_priority;
  if (a_activated != b_activated) return a_activated > b_activated;
  return a < b;
}

}  // namespace

Player::Player(Scheduler* owner, std::unique_ptr<FrameSource> source,
               Timeline timeline, const wt_sink& sink)
    : owner_(owner),
      source_(std::move(source)),
      sink_(sink),
      timeline_(std::move(timeline)) {}

// --- Lifecycle ---------------------------------------------------------------

void Scheduler::DefaultOptions(wt_sched_options* out) {
  const uint32_t cores = std::thread::hardware_concurrency();
  out->worker_count = std::clamp<uint32_t>(cores / 2, 1, 4);
  out->max_fps = 60;
  out->max_concurrent = 0;
}

bool Scheduler::OptionsValid(const wt_sched_options& o) {
  return o.worker_count >= 1 && o.worker_count <= WT_SCHED_MAX_WORKERS &&
         o.max_fps >= 1 && o.max_fps <= 240;
}

Scheduler::Scheduler(const wt_sched_options& options,
                     const wt_sched_hooks& hooks)
    : hooks_(hooks),
      period_(PeriodFor(options.max_fps)),
      max_concurrent_(options.max_concurrent) {}

wt_status Scheduler::Create(const wt_sched_options& options,
                            const wt_sched_hooks& hooks,
                            std::unique_ptr<Scheduler>* out) {
  out->reset();
  if (!OptionsValid(options)) return WT_ERR_INVALID_ARGUMENT;
  std::unique_ptr<Scheduler> s(new (std::nothrow) Scheduler(options, hooks));
  if (s == nullptr) return WT_ERR_NO_MEMORY;
  if (pthread_create(&s->clock_, nullptr, &Scheduler::ClockMain, s.get()) !=
      0) {
    return WT_ERR_NO_MEMORY;
  }
  s->clock_started_ = true;
  {
    std::lock_guard<std::mutex> ol(s->options_mu_);
    {
      std::lock_guard<std::mutex> lk(s->mu_);
      s->worker_target_ = options.worker_count;
    }
    for (uint32_t i = 0; i < options.worker_count; ++i) {
      // The destructor joins whatever did start.
      if (!s->StartWorker()) return WT_ERR_NO_MEMORY;
    }
  }
  *out = std::move(s);
  return WT_OK;
}

Scheduler::~Scheduler() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
  }
  clock_cv_.notify_all();
  work_cv_.notify_all();
  if (clock_started_) pthread_join(clock_, nullptr);
  {
    std::lock_guard<std::mutex> ol(options_mu_);
    JoinWorkers(0);
  }
  // No thread is left, so no lock is needed.
  for (Player* p : players_) delete p;
  players_.clear();
}

bool Scheduler::StartWorker() {
  std::unique_ptr<Worker> w(new (std::nothrow) Worker);
  if (w == nullptr) return false;
  w->owner = this;
  w->index = static_cast<uint32_t>(workers_.size());
  if (pthread_create(&w->thread, nullptr, &Scheduler::WorkerMain, w.get()) !=
      0) {
    return false;
  }
  workers_.push_back(std::move(w));
  std::lock_guard<std::mutex> lk(mu_);
  worker_count_ = static_cast<uint32_t>(workers_.size());
  return true;
}

void Scheduler::JoinWorkers(size_t keep) {
  for (size_t i = keep; i < workers_.size(); ++i) {
    pthread_join(workers_[i]->thread, nullptr);
  }
  if (workers_.size() > keep) workers_.resize(keep);
  std::lock_guard<std::mutex> lk(mu_);
  worker_count_ = static_cast<uint32_t>(workers_.size());
}

wt_status Scheduler::SetOptions(const wt_sched_options& options) {
  if (!OptionsValid(options)) return WT_ERR_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> ol(options_mu_);
  {
    std::lock_guard<std::mutex> lk(mu_);
    period_ = PeriodFor(options.max_fps);
    max_concurrent_ = options.max_concurrent;
    worker_target_ = options.worker_count;
    ranks_dirty_ = true;
  }
  clock_cv_.notify_one();
  work_cv_.notify_all();
  // Workers past the new count exit once their frame in flight is done.
  if (workers_.size() > options.worker_count) JoinWorkers(options.worker_count);
  while (workers_.size() < options.worker_count) {
    if (!StartWorker()) {
      std::lock_guard<std::mutex> lk(mu_);
      worker_target_ = static_cast<uint32_t>(workers_.size());
      return WT_ERR_NO_MEMORY;
    }
  }
  return WT_OK;
}

wt_sched_stats Scheduler::Stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  wt_sched_stats s = stats_;
  s.players = static_cast<uint32_t>(players_.size());
  s.active = 0;
  for (const Player* p : players_) s.active += p->active_ ? 1 : 0;
  s.running = running_count_;
  s.workers = worker_count_;
  return s;
}

// --- Players -----------------------------------------------------------------

wt_status Scheduler::AddPlayer(std::unique_ptr<FrameSource>* source, bool loop,
                               const wt_sink& sink, Player** out) {
  *out = nullptr;
  if (*source == nullptr || sink.acquire == nullptr ||
      sink.release == nullptr) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  const uint32_t frames = (*source)->frame_count();
  if (frames == 0) return WT_ERR_INVALID_ARGUMENT;
  std::vector<Nanos> durations(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    durations[i] = std::chrono::milliseconds((*source)->frame_duration_ms(i));
  }
  // The initializer (and so the move from *source) only runs once the
  // allocation succeeded, so on failure the caller keeps its source.
  Player* p = new (std::nothrow) Player(
      this, std::move(*source), Timeline(std::move(durations), loop), sink);
  if (p == nullptr) return WT_ERR_NO_MEMORY;
  {
    std::lock_guard<std::mutex> lk(mu_);
    players_.push_back(p);
  }
  clock_cv_.notify_one();  // The first frame is a request.
  *out = p;
  return WT_OK;
}

void Scheduler::RemovePlayer(Player* p) {
  {
    std::unique_lock<std::mutex> lk(mu_);
    p->removing_ = true;
    if (p->in_flight_ && !p->executing_) {
      queue_.erase(std::find(queue_.begin(), queue_.end(), p));
      p->in_flight_ = false;
    }
    done_cv_.wait(lk, [p] { return !p->in_flight_; });
    players_.erase(std::find(players_.begin(), players_.end(), p));
    // A frozen player may take its place.
    if (p->timeline_.running()) ranks_dirty_ = true;
  }
  clock_cv_.notify_one();
  delete p;  // Destroys the source (and its wt_anim) without the lock.
}

void Scheduler::SetActive(Player* p, bool active) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (p->active_ == active) return;
    p->active_ = active;
    if (active) p->activated_ = ++activations_;
    ranks_dirty_ = true;
  }
  clock_cv_.notify_one();
}

void Scheduler::SetPriority(Player* p, uint64_t priority) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (p->priority_ == priority) return;
    p->priority_ = priority;
    ranks_dirty_ = true;
  }
  clock_cv_.notify_one();
}

wt_status Scheduler::Seek(Player* p, uint32_t frame) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (frame >= p->timeline_.frame_count()) return WT_ERR_INVALID_ARGUMENT;
    p->timeline_.Seek(frame);
    ranks_dirty_ = true;  // An ended player may play again.
  }
  clock_cv_.notify_one();
  return WT_OK;
}

void Scheduler::Redraw(Player* p) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    p->parked_ = false;
    ++p->target_generation_;
    p->retry_at_ = TimePoint::min();
    p->timeline_.Redraw(SchedClock::now());
  }
  clock_cv_.notify_one();
}

bool Scheduler::IsRunning(const Player* p) const {
  std::lock_guard<std::mutex> lk(mu_);
  return p->timeline_.running();
}

// --- Clock -------------------------------------------------------------------

void* Scheduler::ClockMain(void* self) {
  static_cast<Scheduler*>(self)->ClockLoop();
  return nullptr;
}

void Scheduler::ClockLoop() {
  NameThread("wt-clock");
  if (hooks_.thread_start != nullptr) {
    hooks_.thread_start(hooks_.context, "wt-clock");
  }
  {
    std::unique_lock<std::mutex> lk(mu_);
    while (!quit_) {
      ++stats_.wakeups;
      const TimePoint now = SchedClock::now();
      if (ranks_dirty_) Rerank(now);
      const TimePoint wake = Round(now);
      clock_wake_ = wake;
      if (wake == TimePoint::max()) {
        clock_cv_.wait(lk);
      } else if (wake > now) {
        clock_cv_.wait_until(lk, wake);
      }
      clock_wake_ = TimePoint::min();  // Awake: workers needn't wake it.
    }
  }
  if (hooks_.thread_stop != nullptr) hooks_.thread_stop(hooks_.context);
}

void Scheduler::Rerank(TimePoint now) {
  scratch_.clear();
  for (Player* p : players_) {
    p->winner_ = false;
    if (p->active_ && !p->removing_ && p->timeline_.animates()) {
      scratch_.push_back(p);
    }
  }
  const auto better = [](const Player* a, const Player* b) {
    return Better(a, a->priority_, a->activated_, b, b->priority_,
                  b->activated_);
  };
  size_t winners = scratch_.size();
  if (max_concurrent_ != 0 && winners > max_concurrent_) {
    winners = max_concurrent_;
    const auto nth =
        scratch_.begin() + static_cast<std::ptrdiff_t>(winners - 1);
    std::nth_element(scratch_.begin(), nth, scratch_.end(), better);
  }
  for (size_t i = 0; i < winners; ++i) scratch_[i]->winner_ = true;
  for (Player* p : players_) {
    if (p->winner_) {
      p->timeline_.Start(now);
    } else {
      p->timeline_.Stop(now);
    }
  }
  std::sort(players_.begin(), players_.end(),
            [&better](const Player* a, const Player* b) {
              if (a->winner_ != b->winner_) return a->winner_;
              return better(a, b);
            });
  running_count_ = static_cast<uint32_t>(winners);
  ranks_dirty_ = false;
}

TimePoint Scheduler::Round(TimePoint now) {
  const Nanos early = period_ / 2;
  const Nanos stall = std::max<Nanos>(2 * period_, kMinStall);
  const bool round_open = now + kRoundSlack >= last_round_ + period_;
  TimePoint request_wake = TimePoint::max();
  TimePoint playback_wake = TimePoint::max();
  bool played = false;
  for (Player* p : players_) {
    if (p->in_flight_ || p->removing_ || p->parked_) continue;
    Timeline& t = p->timeline_;
    if (t.dirty()) {
      if (now < p->retry_at_) {
        request_wake = std::min(request_wake, p->retry_at_);
      } else {
        Dispatch(p, t.PlanRequest());
      }
      continue;
    }
    if (!t.running() || !t.animates()) continue;
    if (now < p->retry_at_) {
      playback_wake = std::min(playback_wake, p->retry_at_);
      continue;
    }
    if (round_open) {
      FrameJob job;
      if (t.PlanPlayback(now, early, stall, &job)) {
        Dispatch(p, job);
        played = true;
        continue;
      }
      if (t.ended()) {
        ranks_dirty_ = true;  // Its slot frees up.
        continue;
      }
    }
    playback_wake = std::min(playback_wake, t.due() - early);
  }
  if (played) {
    last_round_ = now;
    ++stats_.ticks;
  }
  if (ranks_dirty_) return now;
  TimePoint wake = request_wake;
  if (playback_wake != TimePoint::max()) {
    wake = std::min(wake, std::max(playback_wake, last_round_ + period_));
  }
  return wake;
}

void Scheduler::Dispatch(Player* p, const FrameJob& job) {
  p->job_ = job;
  p->job_target_generation_ = p->target_generation_;
  p->in_flight_ = true;
  queue_.push_back(p);
  work_cv_.notify_one();
}

void Scheduler::WakeClockBy(TimePoint when) {
  if (when < clock_wake_) clock_cv_.notify_one();
}

// --- Workers -----------------------------------------------------------------

void* Scheduler::WorkerMain(void* worker) {
  Worker* w = static_cast<Worker*>(worker);
  w->owner->WorkerLoop(w->index);
  return nullptr;
}

void Scheduler::WorkerLoop(uint32_t index) {
  const char* name = kWorkerNames[index];
  NameThread(name);
  if (hooks_.thread_start != nullptr) hooks_.thread_start(hooks_.context, name);
  {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
      work_cv_.wait(lk, [this, index] {
        return quit_ || index >= worker_target_ || !queue_.empty();
      });
      if (quit_ || index >= worker_target_) break;
      Player* p = queue_.front();
      queue_.pop_front();
      p->executing_ = true;
      const FrameJob job = p->job_;
      lk.unlock();
      const JobResult result = Execute(p, job);
      const TimePoint done = SchedClock::now();
      lk.lock();
      Finish(p, job, result, done);
    }
  }
  if (hooks_.thread_stop != nullptr) hooks_.thread_stop(hooks_.context);
}

void Scheduler::Report(Player* p, wt_status status) {
  if (p->reported_error_ || p->sink_.error == nullptr) return;
  p->reported_error_ = true;
  p->sink_.error(p->sink_.context, status);
}

Scheduler::JobResult Scheduler::Execute(Player* p, const FrameJob& job) {
  JobResult r;
  FrameSource& src = *p->source_;
  const uint32_t frames = src.frame_count();
  const bool loop = p->timeline_.loop();  // Immutable.
  uint32_t cursor = job.from;
  wt_status skip_error = WT_OK;
  if (!job.seek) {
    for (uint32_t i = 0; i < job.skips; ++i) {
      if (cursor >= frames) {
        if (!loop || src.Seek(0) != WT_OK) break;
        cursor = 0;
      }
      const wt_status s = src.Skip();
      if (s == WT_END) break;  // Out of step; the seek below fixes it.
      ++cursor;                // Errors advance the cursor too.
      ++r.skipped;
      if (s < 0 && skip_error == WT_OK) skip_error = s;
    }
  }
  if (job.seek || cursor != job.target) {
    const wt_status s = src.Seek(job.target);
    if (s != WT_OK) {
      // The target is always in range, so this is a bug; don't retry it.
      r.outcome.drawn = true;
      Report(p, s < 0 ? s : WT_ERR_INTERNAL);
      return r;
    }
    cursor = job.target;
  }
  r.outcome.cursor = cursor;
  r.outcome.cursor_known = true;
  if (skip_error != WT_OK) Report(p, skip_error);

  wt_buffer buffer{};
  r.acquire = p->sink_.acquire(p->sink_.context, &buffer);
  if (r.acquire != WT_OK) return r;
  wt_frame_info frame{};
  wt_status s = src.Draw(buffer, &frame);
  // A rejected buffer doesn't advance the cursor; everything else does.
  if (s != WT_ERR_INVALID_ARGUMENT) ++cursor;
  if (s == WT_END) s = WT_ERR_INTERNAL;  // The cursor was just positioned.
  p->sink_.release(p->sink_.context, s, &frame);
  r.outcome.drawn = true;
  r.outcome.cursor = cursor;
  r.drawn_ok = s == WT_OK;
  if (s < 0) Report(p, s);
  return r;
}

void Scheduler::Finish(Player* p, const FrameJob& job, const JobResult& r,
                       TimePoint done) {
  p->in_flight_ = false;
  p->executing_ = false;
  stats_.frames_skipped += r.skipped;
  if (r.drawn_ok) ++stats_.frames_drawn;
  // A redraw (a new target) that came in while the job ran wins over what
  // the job found: it may have drawn into the old target, or found none.
  const bool redrawn = p->target_generation_ != p->job_target_generation_;
  if (redrawn) {
    // Redraw already cleared parked_ and retry_at_.
  } else if (r.acquire == WT_NO_TARGET) {
    p->parked_ = true;
  } else if (r.acquire == WT_BUSY) {
    p->retry_at_ = done + period_;
  } else if (r.acquire < 0) {
    p->retry_at_ = done + kErrorRetry;
  } else {
    p->retry_at_ = TimePoint::min();
  }
  p->timeline_.Complete(job, r.outcome, done);
  // Complete set the clock from the job; ask for the new target's frame
  // again on top of it.
  if (redrawn) p->timeline_.Redraw(done);
  if (p->removing_) {
    done_cv_.notify_all();
    return;
  }
  if (p->parked_) return;
  const Timeline& t = p->timeline_;
  if (t.dirty()) {
    WakeClockBy(std::max(p->retry_at_, done));
  } else if (t.running() && t.animates()) {
    WakeClockBy(std::max(p->retry_at_, t.due() - period_ / 2));
  }
}

}  // namespace wt
