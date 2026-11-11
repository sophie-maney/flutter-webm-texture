// wt::Scheduler: one clock thread and a pool of workers that play every
// animation of the process (T3.3, ADR 0017). The C ABI (wt_sched_*,
// wt_player_* in wt_api.h, documented there) is a thin layer over it.
//
// Threads:
// - The clock thread plans. It sleeps until the earliest of: a pending
//   request, the next due frame (rounded to at most max_fps rounds a
//   second), a retry. With nothing to play it waits without a timeout.
// - Workers run jobs from a FIFO queue: position the cursor, acquire a sink
//   buffer, draw, release. Each player is in the queue (or being run) at most
//   once, so its FrameSource and sink are used by one thread at a time.
// - The public methods may be called from any thread except the scheduler's
//   own; RemovePlayer, SetOptions (when shrinking) and the destructor wait
//   for frames in flight.
//
// Everything mutable is guarded by mu_; FrameSource and sink calls run
// without it.
#ifndef WT_SCHED_SCHEDULER_H_
#define WT_SCHED_SCHEDULER_H_

#include <pthread.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "api/wt_api.h"
#include "sched/frame_source.h"
#include "sched/timeline.h"

namespace wt {

class Scheduler;

// One animation on a scheduler. Created by Scheduler::AddPlayer and freed by
// Scheduler::RemovePlayer; its fields belong to the scheduler.
class Player {
 public:
  Scheduler* scheduler() const { return owner_; }

 private:
  friend class Scheduler;
  Player(Scheduler* owner, std::unique_ptr<FrameSource> source,
         Timeline timeline, const wt_sink& sink);

  Scheduler* const owner_;
  const std::unique_ptr<FrameSource> source_;
  const wt_sink sink_;

  // Guarded by owner_->mu_.
  Timeline timeline_;
  bool active_ = false;
  uint64_t priority_ = 0;
  uint64_t activated_ = 0;  // Recency: the activation counter when activated.
  bool in_flight_ = false;  // Queued or being run.
  bool executing_ = false;  // A worker took the job off the queue.
  bool removing_ = false;
  bool parked_ = false;             // WT_NO_TARGET: wait for Redraw.
  uint64_t target_generation_ = 0;  // Bumped by Redraw.
  TimePoint retry_at_{};
  FrameJob job_;
  uint64_t job_target_generation_ = 0;
  bool winner_ = false;  // Scratch for Rerank.

  // Touched only by the worker running this player's job.
  bool reported_error_ = false;
};

class Scheduler {
 public:
  // Starts the clock and options.worker_count workers. Fails with
  // WT_ERR_INVALID_ARGUMENT for invalid options and WT_ERR_NO_MEMORY when a
  // thread can't be started.
  static wt_status Create(const wt_sched_options& options,
                          const wt_sched_hooks& hooks,
                          std::unique_ptr<Scheduler>* out);

  // Joins every thread, then frees the players left.
  ~Scheduler();

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  static void DefaultOptions(wt_sched_options* out);
  static bool OptionsValid(const wt_sched_options& options);

  wt_status SetOptions(const wt_sched_options& options);
  wt_sched_stats Stats() const;

  // Adds a player for *source, which it takes on success only. The sink's
  // acquire and release must be set.
  wt_status AddPlayer(std::unique_ptr<FrameSource>* source, bool loop,
                      const wt_sink& sink, Player** out);
  // Waits for the player's frame in flight, then frees it and its source.
  void RemovePlayer(Player* player);

  void SetActive(Player* player, bool active);
  void SetPriority(Player* player, uint64_t priority);
  wt_status Seek(Player* player, uint32_t frame);
  void Redraw(Player* player);

  // Whether the player is playing now (for tests and diagnostics).
  bool IsRunning(const Player* player) const;

 private:
  struct Worker {
    Scheduler* owner = nullptr;
    uint32_t index = 0;
    pthread_t thread{};
  };
  struct JobResult {
    FrameOutcome outcome;
    wt_status acquire = WT_OK;
    bool drawn_ok = false;
    uint32_t skipped = 0;
  };

  Scheduler(const wt_sched_options& options, const wt_sched_hooks& hooks);

  static void* ClockMain(void* self);
  static void* WorkerMain(void* worker);
  void ClockLoop();
  void WorkerLoop(uint32_t index);

  // Starts worker number workers_.size(). Requires options_mu_.
  bool StartWorker();
  // Joins and drops the workers from index `keep` on. Requires options_mu_.
  void JoinWorkers(size_t keep);

  // Requires mu_. Decides which active players run (max_concurrent) and
  // orders players_ for dispatch, most important first.
  void Rerank(TimePoint now);
  // Requires mu_. Dispatches every request and due playback frame, and
  // returns when the clock should look again (TimePoint::max(): only when
  // something changes).
  TimePoint Round(TimePoint now);
  // Requires mu_.
  void Dispatch(Player* player, const FrameJob& job);
  // Runs a job; without mu_.
  JobResult Execute(Player* player, const FrameJob& job);
  // Requires mu_.
  void Finish(Player* player, const FrameJob& job, const JobResult& result,
              TimePoint done);
  // Wakes the clock if `when` is earlier than it planned to look. Requires
  // mu_.
  void WakeClockBy(TimePoint when);
  void Report(Player* player, wt_status status);

  const wt_sched_hooks hooks_;

  // Serialises SetOptions; guards workers_.
  std::mutex options_mu_;
  std::vector<std::unique_ptr<Worker>> workers_;
  bool clock_started_ = false;
  pthread_t clock_{};

  mutable std::mutex mu_;
  std::condition_variable clock_cv_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  bool quit_ = false;
  Nanos period_{};
  uint32_t max_concurrent_ = 0;
  uint32_t worker_target_ = 0;
  uint32_t worker_count_ = 0;
  std::vector<Player*> players_;
  std::vector<Player*> scratch_;
  std::deque<Player*> queue_;
  bool ranks_dirty_ = false;
  uint64_t activations_ = 0;
  uint32_t running_count_ = 0;
  TimePoint last_round_{};
  // When the sleeping clock will look next.
  TimePoint clock_wake_ = TimePoint::max();
  wt_sched_stats stats_{};
};

}  // namespace wt

#endif  // WT_SCHED_SCHEDULER_H_
