// Fakes shared by the scheduler tests (sched_test.cc, sched_stress_test.cc):
// a FrameSource with no decoder and a sink that records what it is asked to
// do and checks the scheduler's promises (one callback at a time per player,
// release after acquire on the same thread, nothing after destroy).
#ifndef WT_NATIVE_TEST_SCHED_UTIL_H_
#define WT_NATIVE_TEST_SCHED_UTIL_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "sched/frame_source.h"
#include "wt_api.h"

namespace wt_test {

// Counts overlapping calls; any overlap is a scheduler bug.
class Exclusive {
 public:
  class Scope {
   public:
    explicit Scope(Exclusive* e) : e_(e) {
      if (e_->busy_.fetch_add(1) != 0) e_->overlaps_.fetch_add(1);
    }
    ~Scope() { e_->busy_.fetch_sub(1); }

   private:
    Exclusive* e_;
  };
  int overlaps() const { return overlaps_.load(); }

 private:
  std::atomic<int> busy_{0};
  std::atomic<int> overlaps_{0};
};

// A frame source with fixed durations and no pixels to decode: Write puts
// the frame index into the first four bytes of the buffer.
class FakeSource final : public wt::FrameSource {
 public:
  // Shared with the test, which may outlive the source.
  struct Log {
    Exclusive exclusive;
    std::atomic<uint32_t> seeks{0};
    std::atomic<uint32_t> skips{0};
    std::atomic<uint32_t> draws{0};
    // Prepare and Skip fail with WT_ERR_DECODE for this frame (UINT32_MAX:
    // never).
    std::atomic<uint32_t> broken_frame{UINT32_MAX};
  };

  FakeSource(std::vector<uint32_t> durations_ms, std::shared_ptr<Log> log)
      : durations_(std::move(durations_ms)), log_(std::move(log)) {}

  uint32_t frame_count() const override {
    return static_cast<uint32_t>(durations_.size());
  }
  uint32_t frame_duration_ms(uint32_t index) const override {
    return durations_[index];
  }
  wt_status Seek(uint32_t index) override {
    Exclusive::Scope scope(&log_->exclusive);
    if (index >= frame_count()) return WT_ERR_INVALID_ARGUMENT;
    log_->seeks.fetch_add(1);
    cursor_ = index;
    return WT_OK;
  }
  wt_status Skip(const wt::CancelFlag&) override {
    Exclusive::Scope scope(&log_->exclusive);
    if (cursor_ >= frame_count()) return WT_END;
    log_->skips.fetch_add(1);
    // Like a real skip, a broken frame fails to decode (and still advances).
    return cursor_++ == log_->broken_frame.load() ? WT_ERR_DECODE : WT_OK;
  }
  wt_status Prepare(const wt::CancelFlag&, wt_frame_info* out) override {
    Exclusive::Scope scope(&log_->exclusive);
    std::memset(out, 0, sizeof(*out));
    if (cursor_ >= frame_count()) return WT_END;
    const uint32_t index = cursor_++;
    log_->draws.fetch_add(1);
    if (index == log_->broken_frame.load()) return WT_ERR_DECODE;
    prepared_ = index;
    out->index = index;
    out->duration_ms = durations_[index];
    return WT_OK;
  }
  wt_status Write(const wt_buffer& dst) override {
    Exclusive::Scope scope(&log_->exclusive);
    if (dst.pixels == nullptr || dst.size < 4) return WT_ERR_INVALID_ARGUMENT;
    std::memcpy(dst.pixels, &prepared_, sizeof(prepared_));
    return WT_OK;
  }

 private:
  const std::vector<uint32_t> durations_;
  const std::shared_ptr<Log> log_;
  uint32_t cursor_ = 0;
  uint32_t prepared_ = 0;  // The frame Prepare kept for Write.
};

// A wt_sink that records the frames it shows.
class FakeSink {
 public:
  using Clock = std::chrono::steady_clock;

  // A width x height RGBA buffer. With fake_pixels, the source is a
  // FakeSource and release checks the frame index it wrote.
  explicit FakeSink(uint32_t width = 4, uint32_t height = 4,
                    bool fake_pixels = true)
      : pixels_(size_t{width} * height * 4, 0),
        stride_(width * 4),
        fake_pixels_(fake_pixels) {}

  wt_sink sink() {
    wt_sink s{};
    s.context = this;
    s.acquire = &FakeSink::Acquire;
    s.release = &FakeSink::Release;
    s.error = &FakeSink::Error;
    return s;
  }

  // What acquire answers (WT_OK by default). With `busy_for` > 0, the next
  // that many acquires answer WT_BUSY first.
  void set_answer(wt_status answer) { answer_.store(answer); }
  void set_busy_for(int n) { busy_for_.store(n); }
  // While closed, acquire blocks until Open().
  void Close() {
    std::lock_guard<std::mutex> lk(gate_mu_);
    gate_open_ = false;
  }
  void Open() {
    {
      std::lock_guard<std::mutex> lk(gate_mu_);
      gate_open_ = true;
    }
    gate_cv_.notify_all();
  }
  // Waits until a thread is blocked in acquire (after Close).
  bool WaitBlocked(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(gate_mu_);
    return gate_cv_.wait_for(lk, timeout, [this] { return blocked_ > 0; });
  }
  // Marks the player destroyed: any later callback is counted as a bug.
  void MarkDestroyed() { destroyed_.store(true); }

  int acquires() const { return acquires_.load(); }
  int errors() const { return errors_.load(); }
  wt_status first_error() const { return first_error_.load(); }
  int late_calls() const { return late_calls_.load(); }
  int protocol_errors() const { return protocol_errors_.load(); }
  int overlaps() const { return exclusive_.overlaps(); }

  // The frames shown so far, in order.
  std::vector<uint32_t> shown() const {
    std::lock_guard<std::mutex> lk(mu_);
    return shown_;
  }
  size_t shown_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return shown_.size();
  }
  // Waits until at least n frames were shown.
  bool WaitShown(size_t n, std::chrono::milliseconds timeout) const {
    return WaitUntil(
        [n](const std::vector<uint32_t>& shown) { return shown.size() >= n; },
        timeout);
  }
  // Waits until done(shown()) holds.
  template <typename Done>
  bool WaitUntil(Done done, std::chrono::milliseconds timeout) const {
    const auto end = Clock::now() + timeout;
    while (Clock::now() < end) {
      if (done(shown())) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done(shown());
  }

 private:
  static wt_status Acquire(void* context, wt_buffer* out) {
    auto* self = static_cast<FakeSink*>(context);
    if (self->destroyed_.load()) self->late_calls_.fetch_add(1);
    Exclusive::Scope scope(&self->exclusive_);
    self->acquires_.fetch_add(1);
    {
      std::unique_lock<std::mutex> lk(self->gate_mu_);
      ++self->blocked_;
      self->gate_cv_.notify_all();
      self->gate_cv_.wait(lk, [self] { return self->gate_open_; });
      --self->blocked_;
    }
    int busy = self->busy_for_.load();
    while (busy > 0 && !self->busy_for_.compare_exchange_weak(busy, busy - 1)) {
    }
    if (busy > 0) return WT_BUSY;
    const wt_status answer = self->answer_.load();
    if (answer != WT_OK) return answer;
    if (self->in_attempt_.exchange(true)) self->protocol_errors_.fetch_add(1);
    self->attempt_thread_ = std::this_thread::get_id();
    out->pixels = self->pixels_.data();
    out->size = self->pixels_.size();
    out->stride = self->stride_;
    out->format = WT_PIXEL_FORMAT_RGBA8888;
    return WT_OK;
  }

  static void Release(void* context, wt_status status,
                      const wt_frame_info* frame) {
    auto* self = static_cast<FakeSink*>(context);
    if (self->destroyed_.load()) self->late_calls_.fetch_add(1);
    Exclusive::Scope scope(&self->exclusive_);
    if (!self->in_attempt_.exchange(false) ||
        self->attempt_thread_ != std::this_thread::get_id()) {
      self->protocol_errors_.fetch_add(1);
    }
    if (status != WT_OK) return;
    uint32_t index = frame->index;
    if (self->fake_pixels_) {
      std::memcpy(&index, self->pixels_.data(), sizeof(index));
      if (index != frame->index) self->protocol_errors_.fetch_add(1);
    }
    std::lock_guard<std::mutex> lk(self->mu_);
    self->shown_.push_back(index);
  }

  static void Error(void* context, wt_status status) {
    auto* self = static_cast<FakeSink*>(context);
    if (self->destroyed_.load()) self->late_calls_.fetch_add(1);
    if (self->errors_.fetch_add(1) == 0) self->first_error_.store(status);
  }

  std::vector<uint8_t> pixels_;
  const uint32_t stride_;
  const bool fake_pixels_;
  Exclusive exclusive_;
  std::atomic<bool> in_attempt_{false};
  std::thread::id attempt_thread_;
  std::atomic<wt_status> answer_{WT_OK};
  std::atomic<int> busy_for_{0};
  std::atomic<bool> destroyed_{false};
  std::atomic<int> acquires_{0};
  std::atomic<int> errors_{0};
  std::atomic<wt_status> first_error_{WT_OK};
  std::atomic<int> late_calls_{0};
  std::atomic<int> protocol_errors_{0};

  mutable std::mutex mu_;
  std::vector<uint32_t> shown_;

  std::mutex gate_mu_;
  std::condition_variable gate_cv_;
  bool gate_open_ = true;
  int blocked_ = 0;
};

}  // namespace wt_test

#endif  // WT_NATIVE_TEST_SCHED_UTIL_H_
