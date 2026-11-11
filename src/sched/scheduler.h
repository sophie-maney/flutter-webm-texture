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
//   own; RemovePlayer, Retarget, RemoveGroup, SetOptions (when shrinking) and
//   the destructor wait for frames (or presents) in flight.
// - Workers also run presents (below), ahead of frame jobs.
//
// Present groups (T4.2, ADR 0020): players can belong to a Group whose
// PresentTarget (an atlas page) shows all their frames at once. The jobs a
// round dispatches for a group's players form a batch; when the last of them
// is done, a worker calls the target's Present, at most once per period per
// group and never twice at once. Retarget moves a player to another sink
// (and group) without losing its place in the animation.
//
// Beats (T4.3, ADR 0021): a group's players show their playback frames on a
// shared grid of times, the group's beat: its members' shortest frame, at
// least a clock period, counted from the steady clock's epoch so that groups
// with the same beat share it. A frame is drawn at the beat nearest to when
// it is due. Players that started at different times thus fill the same
// rounds, and the group presents once per content frame instead of at every
// tick. No two frames of a player land on one beat, since none is shorter
// than the beat.
//
// Everything mutable is guarded by mu_; FrameSource, sink and PresentTarget
// calls run without it.
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

class Group;
class Player;
class Scheduler;

// Shows the frames of a group's players together (an atlas page). The
// scheduler calls it on its workers, without its lock.
class PresentTarget {
 public:
  virtual ~PresentTarget() = default;

  // Shows the latest frames of the group. Called after a round's batch for
  // the group finished and after Scheduler::RequestPresent, never twice at
  // once for one group, and never after RemoveGroup returns. `start` is when
  // the scheduler started this present; the starts of one group's presents
  // are at least a clock period (less 1 ms of timer slack) apart. Returns
  // WT_OK, WT_BUSY (try again a period later), WT_NO_TARGET (wait for
  // RequestPresent) or an error (try again in about 100 ms).
  virtual wt_status Present(TimePoint start) = 0;

  // `player`, a member of the group, is being destroyed: its frame in flight
  // is done and its sink won't be called again. Runs on the thread calling
  // RemovePlayer, without the scheduler's lock, before the player is freed.
  virtual void Leave(Player* player) = 0;
};

// One animation on a scheduler. Created by Scheduler::AddPlayer and freed by
// Scheduler::RemovePlayer; its fields belong to the scheduler.
class Player {
 public:
  Scheduler* scheduler() const { return owner_; }
  // Only its immutable parts (frame count, durations, output size) may be
  // used from outside the scheduler.
  const FrameSource& source() const { return *source_; }

 private:
  friend class Scheduler;
  Player(Scheduler* owner, std::unique_ptr<FrameSource> source,
         Timeline timeline, const wt_sink& sink);

  Scheduler* const owner_;
  const std::unique_ptr<FrameSource> source_;

  // Written under owner_->mu_ while no job is in flight (Retarget); read by
  // the worker running the player's job.
  wt_sink sink_;

  // Guarded by owner_->mu_.
  Group* group_ = nullptr;
  uint64_t job_round_ = 0;  // The round that dispatched the job in flight.
  bool retargeting_ = false;
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

// A set of players presented together. Created by Scheduler::AddGroup and
// freed by Scheduler::RemoveGroup; its fields belong to the scheduler.
class Group {
 private:
  friend class Scheduler;
  struct Batch {
    uint64_t round = 0;
    uint32_t remaining = 0;
  };
  explicit Group(PresentTarget* target) : target_(target) {}

  PresentTarget* const target_;

  // Guarded by the scheduler's mu_.
  std::vector<Batch> batches_;  // Rounds with jobs still in flight.
  uint32_t members_ = 0;
  bool pending_ = false;     // Needs a present.
  bool queued_ = false;      // In present_queue_.
  bool presenting_ = false;  // A worker runs Present.
  bool parked_ = false;      // WT_NO_TARGET: wait for RequestPresent.
  bool removing_ = false;
  TimePoint not_before_{};  // Rate limit and retries.
  // The shortest frame of the members that animate (Nanos::max(): none).
  Nanos min_frame_ = Nanos::max();
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

  // Moves the player to `sink` and `group` (nullptr: none), waiting for its
  // frame in flight, and asks for the frame that should be showing (as
  // Redraw). Fails with WT_ERR_INVALID_ARGUMENT, changing nothing, when the
  // sink lacks acquire or release, the player is being removed or
  // retargeted, or its group isn't `expect`. *previous (may be null)
  // receives the sink it had.
  wt_status Retarget(Player* player, const wt_sink& sink, Group* group,
                     Group* expect, wt_sink* previous);

  // Adds a group presented by `target`, which must outlive it.
  wt_status AddGroup(PresentTarget* target, Group** out);
  // Waits for a present in flight, then frees the group. It must have no
  // members (Retarget them away first).
  wt_status RemoveGroup(Group* group);
  // Asks for a present soon (within the rate limit), e.g. for a new target.
  // Also ends a WT_NO_TARGET wait.
  void RequestPresent(Group* group);

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
  // Requires mu_. The group's beat: its members' shortest frame, at least a
  // period.
  Nanos BeatOf(const Group* group) const;
  // Requires mu_. When the round that plays the running player's next frame
  // should start: half a period before it is due, or for a group member, the
  // beat nearest to it.
  TimePoint PlaybackWake(const Player* player) const;
  // Requires mu_. Recomputes the group's shortest member frame.
  void UpdateBeat(Group* group);
  void Report(Player* player, wt_status status);
  // Requires mu_. One job of `round` for the group is done (or dropped).
  void CompleteBatch(Group* group, uint64_t round, TimePoint now);
  // Requires mu_. Queues the group's present if it needs one and may have
  // one now; otherwise makes sure the clock looks again in time.
  void MaybeQueuePresent(Group* group, TimePoint now);
  // Requires mu_ (unlocked around Present). Runs one queued present.
  void RunPresent(std::unique_lock<std::mutex>* lk);

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
  std::vector<Group*> groups_;
  std::deque<Group*> present_queue_;
  uint64_t round_seq_ = 0;  // Numbers every Round() call.
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
