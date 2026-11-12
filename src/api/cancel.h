// wt::CancelFlag: how a thread asks a long call running on another thread to
// stop early (T6.1, S-05, ADR 0027).
//
// The scheduler gives each player one flag. Removing, retargeting or moving a
// player off a worker that is going away sets it, and the decode under way
// checks it between frames of a catch-up, so the caller waits for at most one
// frame instead of the whole catch-up. A call that sees the flag returns
// kCancelled and leaves its object as if it had stopped before the frame it
// was working towards: the next call carries on from there.
//
// Thread: Cancel and Reset from the thread that owns the flag (the scheduler,
// under its lock); cancelled() from any thread.
#ifndef WT_API_CANCEL_H_
#define WT_API_CANCEL_H_

#include <atomic>

#include "api/wt_api.h"

namespace wt {

// Internal status: the call stopped early because its CancelFlag was set.
// Never returned through the C ABI (only internal callers pass a flag), and
// positive, so code that treats negative statuses as errors doesn't report it.
constexpr wt_status kCancelled = 1000;

class CancelFlag {
 public:
  bool cancelled() const { return flag_.load(std::memory_order_relaxed); }
  void Cancel() { flag_.store(true, std::memory_order_relaxed); }
  void Reset() { flag_.store(false, std::memory_order_relaxed); }

 private:
  std::atomic<bool> flag_{false};
};

// Whether `flag` (may be null: never cancelled) is set.
inline bool IsCancelled(const CancelFlag* flag) {
  return flag != nullptr && flag->cancelled();
}

}  // namespace wt

#endif  // WT_API_CANCEL_H_
