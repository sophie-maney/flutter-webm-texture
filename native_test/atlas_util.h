// Fakes shared by the atlas tests (atlas_test.cc, atlas_stress_test.cc):
// a frame source that paints every pixel with a value naming the player and
// the frame, a page target that cycles through buffers full of stale
// garbage (as a real buffer queue does), and a present observer that checks
// every live slot of every present pixel by pixel.
#ifndef WT_NATIVE_TEST_ATLAS_UTIL_H_
#define WT_NATIVE_TEST_ATLAS_UTIL_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "atlas/compositor.h"
#include "sched/frame_source.h"
#include "sched/scheduler.h"
#include "sched_util.h"
#include "wt_api.h"

namespace wt_test {

// The 32-bit pixel PatternSource writes: never 0, so a cleared pixel is told
// apart from content.
inline uint32_t Pattern(uint32_t tag, uint32_t frame) {
  return 0x80000000u | ((tag & 0x7ffffu) << 12) | (frame & 0xfffu);
}

// A frame source of a fixed output size that fills every pixel of a frame
// with Pattern(tag, frame).
class PatternSource final : public wt::FrameSource {
 public:
  struct Log {
    Exclusive exclusive;
    std::atomic<uint32_t> draws{0};
    // The last frame drawn successfully (UINT32_MAX: none).
    std::atomic<uint32_t> last_drawn{UINT32_MAX};
  };

  PatternSource(uint32_t tag, uint32_t width, uint32_t height,
                std::vector<uint32_t> durations_ms, std::shared_ptr<Log> log)
      : tag_(tag),
        width_(width),
        height_(height),
        durations_(std::move(durations_ms)),
        log_(std::move(log)) {}

  void GetOutputSize(uint32_t* w, uint32_t* h) const override {
    *w = width_;
    *h = height_;
  }
  uint32_t frame_count() const override {
    return static_cast<uint32_t>(durations_.size());
  }
  uint32_t frame_duration_ms(uint32_t index) const override {
    return durations_[index];
  }
  wt_status Seek(uint32_t index) override {
    Exclusive::Scope scope(&log_->exclusive);
    if (index >= frame_count()) return WT_ERR_INVALID_ARGUMENT;
    cursor_ = index;
    return WT_OK;
  }
  wt_status Skip() override {
    Exclusive::Scope scope(&log_->exclusive);
    if (cursor_ >= frame_count()) return WT_END;
    ++cursor_;
    return WT_OK;
  }
  wt_status Draw(const wt_buffer& dst, wt_frame_info* out) override {
    Exclusive::Scope scope(&log_->exclusive);
    std::memset(out, 0, sizeof(*out));
    if (cursor_ >= frame_count()) return WT_END;
    const size_t row = size_t{width_} * 4;
    if (dst.pixels == nullptr || dst.stride < row ||
        dst.size < size_t{dst.stride} * (height_ - 1) + row) {
      return WT_ERR_INVALID_ARGUMENT;
    }
    const uint32_t index = cursor_++;
    const uint32_t value = Pattern(tag_, index);
    for (uint32_t y = 0; y < height_; ++y) {
      uint8_t* p = dst.pixels + size_t{y} * dst.stride;
      for (uint32_t x = 0; x < width_; ++x) std::memcpy(p + x * 4, &value, 4);
    }
    log_->draws.fetch_add(1);
    log_->last_drawn.store(index);
    out->index = index;
    out->duration_ms = durations_[index];
    return WT_OK;
  }

 private:
  const uint32_t tag_;
  const uint32_t width_;
  const uint32_t height_;
  const std::vector<uint32_t> durations_;
  const std::shared_ptr<Log> log_;
  uint32_t cursor_ = 0;
};

// A wt_atlas_target over `count` buffers of width x height that it hands out
// in turn, each filled with 0xAB garbage once and never cleared, so a slot
// the compositor doesn't rewrite shows up as stale. Checks the protocol:
// present follows acquire on the same thread, no overlapping calls, nothing
// after Retire().
class FakeTarget {
 public:
  FakeTarget(uint32_t width, uint32_t height, uint32_t count = 4,
             uint32_t pad_px = 0,
             wt_pixel_format format = WT_PIXEL_FORMAT_RGBA8888)
      : width_(width),
        height_(height),
        stride_((width + pad_px) * 4),
        format_(format),
        buffers_(count) {
    for (std::vector<uint8_t>& b : buffers_) {
      b.assign(size_t{stride_} * height, 0xAB);
    }
  }

  wt_atlas_target target() {
    wt_atlas_target t{};
    t.context = this;
    t.acquire = &FakeTarget::Acquire;
    t.present = &FakeTarget::Present;
    return t;
  }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  // Acquires answer this (WT_OK by default); busy_for answers WT_BUSY first.
  void set_answer(wt_status s) { answer_.store(s); }
  void set_busy_for(int n) { busy_for_.store(n); }
  // From now on any callback is a bug (the target was replaced).
  void Retire() { retired_.store(true); }

  int presents() const { return presents_.load(); }
  int acquires() const { return acquires_.load(); }
  int late_calls() const { return late_calls_.load(); }
  int protocol_errors() const { return protocol_errors_.load(); }
  int overlaps() const { return exclusive_.overlaps(); }

  // A copy of the last presented buffer (empty before the first).
  std::vector<uint8_t> last() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_;
  }
  uint32_t stride() const { return stride_; }
  void keep_copies(bool keep) { keep_copies_.store(keep); }

  bool WaitPresents(int n, std::chrono::milliseconds timeout) const {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
      if (presents_.load() >= n) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return presents_.load() >= n;
  }

 private:
  static wt_status Acquire(void* context, wt_buffer* out, uint64_t* id) {
    auto* self = static_cast<FakeTarget*>(context);
    if (self->retired_.load()) self->late_calls_.fetch_add(1);
    Exclusive::Scope scope(&self->exclusive_);
    self->acquires_.fetch_add(1);
    int busy = self->busy_for_.load();
    while (busy > 0 && !self->busy_for_.compare_exchange_weak(busy, busy - 1)) {
    }
    if (busy > 0) return WT_BUSY;
    const wt_status answer = self->answer_.load();
    if (answer != WT_OK) return answer;
    if (self->in_attempt_.exchange(true)) self->protocol_errors_.fetch_add(1);
    self->attempt_thread_ = std::this_thread::get_id();
    std::vector<uint8_t>& b = self->buffers_[self->next_];
    self->current_ = self->next_;
    self->next_ = (self->next_ + 1) % self->buffers_.size();
    out->pixels = b.data();
    out->size = b.size();
    out->stride = self->stride_;
    out->format = self->format_;
    *id = 1000 + self->current_;
    return WT_OK;
  }

  static void Present(void* context, wt_status status) {
    auto* self = static_cast<FakeTarget*>(context);
    if (self->retired_.load()) self->late_calls_.fetch_add(1);
    Exclusive::Scope scope(&self->exclusive_);
    if (!self->in_attempt_.exchange(false) ||
        self->attempt_thread_ != std::this_thread::get_id()) {
      self->protocol_errors_.fetch_add(1);
    }
    if (status != WT_OK) return;
    self->presents_.fetch_add(1);
    if (self->keep_copies_.load()) {
      std::lock_guard<std::mutex> lk(self->mu_);
      self->last_ = self->buffers_[self->current_];
    }
  }

  const uint32_t width_;
  const uint32_t height_;
  const uint32_t stride_;
  const wt_pixel_format format_;
  std::vector<std::vector<uint8_t>> buffers_;
  size_t next_ = 0;
  size_t current_ = 0;
  Exclusive exclusive_;
  std::atomic<bool> in_attempt_{false};
  std::thread::id attempt_thread_;
  std::atomic<wt_status> answer_{WT_OK};
  std::atomic<int> busy_for_{0};
  std::atomic<bool> retired_{false};
  std::atomic<int> presents_{0};
  std::atomic<int> acquires_{0};
  std::atomic<int> late_calls_{0};
  std::atomic<int> protocol_errors_{0};
  std::atomic<bool> keep_copies_{false};
  mutable std::mutex mu_;
  std::vector<uint8_t> last_;
};

// Player -> tag, for checking whose pattern a slot shows. Shared by every
// checker of a test.
class Tags {
 public:
  void Set(const wt::Player* p, uint32_t tag) {
    std::lock_guard<std::mutex> lk(mu_);
    tags_[p] = tag;
  }
  // Only if p still has `tag`: once a player is destroyed, another one may
  // get its address (and a tag) before the caller gets here.
  void Erase(const wt::Player* p, uint32_t tag) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = tags_.find(p);
    if (it != tags_.end() && it->second == tag) tags_.erase(it);
  }
  // UINT32_MAX when unknown (a real animation: content isn't checked).
  uint32_t Get(const wt::Player* p) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = tags_.find(p);
    return it == tags_.end() ? UINT32_MAX : it->second;
  }

 private:
  mutable std::mutex mu_;
  std::unordered_map<const wt::Player*, uint32_t> tags_;
};

// Checks every present of one atlas:
// - each live slot that fits the target shows exactly its player's frame
//   (Pattern(tag, frame)) in its content rect, or zeros before its first
//   frame, and zeros in the rest of its outer rect (the gutter), however
//   stale the buffer was;
// - slots that don't fit lie below the target's height;
// - presents of one page start at least min_gap apart (one per tick), by
//   the start time the scheduler gives each present.
class PresentChecker : public wt::AtlasPresentObserver {
 public:
  PresentChecker(const Tags* tags, std::chrono::microseconds min_gap)
      : tags_(tags), min_gap_(min_gap) {}

  void OnPresent(uint32_t page, wt::TimePoint start, const wt_buffer& buf,
                 uint32_t width, uint32_t height,
                 const std::vector<wt::AtlasSlotView>& slots) override {
    const wt::TimePoint now = start;
    std::lock_guard<std::mutex> lk(mu_);
    ++presents_;
    auto last = last_present_.find(page);
    if (last != last_present_.end() && now - last->second < min_gap_) {
      Fail("page %u presented %lld us after the previous present", page,
           static_cast<long long>(
               std::chrono::duration_cast<std::chrono::microseconds>(
                   now - last->second)
                   .count()));
    }
    last_present_[page] = now;
    for (const wt::AtlasSlotView& v : slots) {
      const bool fits = v.outer.x + v.outer.width <= width &&
                        v.outer.y + v.outer.height <= height;
      if (fits != v.written) Fail("slot written %d, fits %d", v.written, fits);
      if (!v.written) continue;
      ++slots_checked_;
      const uint32_t tag = tags_->Get(v.player);
      if (v.has_frame && tag != UINT32_MAX) {
        shown_[v.player] = v.frame;
      }
      for (uint32_t y = v.outer.y; y < v.outer.y + v.outer.height; ++y) {
        const uint8_t* row = buf.pixels + size_t{y} * buf.stride;
        for (uint32_t x = v.outer.x; x < v.outer.x + v.outer.width; ++x) {
          uint32_t px;
          std::memcpy(&px, row + size_t{x} * 4, 4);
          const bool inside = x >= v.inner.x && x < v.inner.x + v.inner.width &&
                              y >= v.inner.y && y < v.inner.y + v.inner.height;
          if (!inside || !v.has_frame) {
            if (px != 0) {
              Fail("page %u (%u,%u): %08x in a cleared area", page, x, y, px);
              return;
            }
          } else if (tag != UINT32_MAX && px != Pattern(tag, v.frame)) {
            Fail("page %u (%u,%u): %08x, expected %08x (tag %u frame %u)", page,
                 x, y, px, Pattern(tag, v.frame), tag, v.frame);
            return;
          }
        }
      }
    }
  }

  int failures() const {
    std::lock_guard<std::mutex> lk(mu_);
    return failures_;
  }
  uint64_t presents() const {
    std::lock_guard<std::mutex> lk(mu_);
    return presents_;
  }
  uint64_t slots_checked() const {
    std::lock_guard<std::mutex> lk(mu_);
    return slots_checked_;
  }
  // The frame the last present showed for a player (UINT32_MAX: none).
  uint32_t shown(const wt::Player* p) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = shown_.find(p);
    return it == shown_.end() ? UINT32_MAX : it->second;
  }
  void Forget(const wt::Player* p) {
    std::lock_guard<std::mutex> lk(mu_);
    shown_.erase(p);
  }

 private:
  template <typename... Args>
  void Fail(const char* fmt, Args... args) {
    if (failures_++ < 5) {
      std::fprintf(stderr, "PresentChecker: ");
      std::fprintf(stderr, fmt, args...);
      std::fprintf(stderr, "\n");
    }
  }

  const Tags* const tags_;
  const std::chrono::microseconds min_gap_;
  mutable std::mutex mu_;
  int failures_ = 0;
  uint64_t presents_ = 0;
  uint64_t slots_checked_ = 0;
  std::map<uint32_t, wt::TimePoint> last_present_;
  std::unordered_map<const wt::Player*, uint32_t> shown_;
};

}  // namespace wt_test

#endif  // WT_NATIVE_TEST_ATLAS_UTIL_H_
