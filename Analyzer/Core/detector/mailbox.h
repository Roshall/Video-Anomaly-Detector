#ifndef ANALYZER_DETECTOR_MAILBOX_H
#define ANALYZER_DETECTOR_MAILBOX_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <opencv2/core.hpp>

#include "inference_types.h"

namespace avs_analyzer {

// Deduplicates inference for monitors that share the same task and frame.
//
// All sinks of a frame receive a task carrying the same SharedInference
// instance. Whichever session acquires the task first claims the inference;
// the other sessions wait for the published results instead of re-running the
// model. If the claiming session does not finish within the wait budget, a
// waiter falls back to running the model locally.
class SharedInference {
 public:
  [[nodiscard]] bool TryClaim() {
    std::lock_guard lock(mutex_);
    if (claimed_ || done_) {
      return false;
    }
    claimed_ = true;
    return true;
  }

  void Publish(std::vector<DL_RESULT> results) {
    {
      std::lock_guard lock(mutex_);
      results_ = std::move(results);
      done_ = true;
    }
    cv_.notify_all();
  }

  // Returns false when no result was published within `timeout`.
  bool AwaitResult(std::vector<DL_RESULT>& out,
                   std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return done_; })) {
      return false;
    }
    out = results_;
    return true;
  }

  [[nodiscard]] bool done() const {
    std::lock_guard lock(mutex_);
    return done_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool claimed_{false};
  bool done_{false};
  std::vector<DL_RESULT> results_;
};

// Bounded per-analyzer task slots shared by one inference session.
//
// Live analyzers run with depth 1: while one frame is in flight, at most one
// newer frame is kept and older pending frames are overwritten. Offline
// analyzers run with depth > 1: extra frames are kept in a bounded backlog so
// the decoder can stay ahead of inference without unbounded memory growth.
class AnalyzerMailboxes {
 public:
  using ImagePtr = std::shared_ptr<const cv::Mat>;
  using ResourceID = std::array<unsigned long, 3>;
  struct Task {
    ResourceID id;
    ImagePtr image;
    // Non-null when several analyzers share this frame; exactly one of their
    // sessions runs inference and publishes the result for the others.
    std::shared_ptr<SharedInference> shared{};
  };

  enum class PushResult {
    kRejected,  // analyzer is not registered
    kQueued,    // task stored, already scheduled
    kSchedule,  // task stored, analyzer must be scheduled
  };

  // `buffered_delta` accounts for replaced/dropped tasks so callers can keep
  // an exact count of buffered tasks without rescanning all slots.
  struct PushOutcome {
    PushResult result{PushResult::kRejected};
    int buffered_delta{0};
  };

  void Add(unsigned long analyzer_id, size_t depth) {
    std::lock_guard lock(mutex_);
    auto& slot = slots_[analyzer_id];
    slot.depth = depth == 0 ? 1 : depth;
  }

  // Returns the number of buffered tasks discarded for this analyzer.
  size_t Remove(unsigned long analyzer_id) {
    std::lock_guard lock(mutex_);
    auto it = slots_.find(analyzer_id);
    if (it == slots_.end()) {
      return 0;
    }
    size_t buffered = it->second.backlog.size();
    if (it->second.has_pending) {
      ++buffered;
    }
    slots_.erase(it);
    return buffered;
  }

  PushOutcome Push(Task task) {
    std::lock_guard lock(mutex_);
    auto it = slots_.find(task.id[0]);
    if (it == slots_.end()) {
      return {PushResult::kRejected, 0};
    }
    Slot& slot = it->second;
    if (!slot.in_flight) {
      const bool was_pending = slot.has_pending;
      slot.pending = std::move(task);
      slot.has_pending = true;
      return {was_pending ? PushResult::kQueued : PushResult::kSchedule,
              was_pending ? 0 : 1};
    }
    if (slot.depth <= 1) {
      const bool had_pending = slot.has_pending;
      slot.pending = std::move(task);
      slot.has_pending = true;
      return {PushResult::kQueued, had_pending ? 0 : 1};
    }
    slot.backlog.push_back(std::move(task));
    int delta = 1;
    while (slot.backlog.size() > slot.depth - 1) {
      slot.backlog.pop_front();
      delta = 0;  // replaced an older buffered task
    }
    return {PushResult::kQueued, delta};
  }

  // Moves the pending task of `analyzer_id` out and marks it in flight.
  bool Acquire(unsigned long analyzer_id, Task& out) {
    std::lock_guard lock(mutex_);
    auto it = slots_.find(analyzer_id);
    if (it == slots_.end() || !it->second.has_pending) {
      return false;
    }
    Slot& slot = it->second;
    out = std::move(slot.pending);
    slot.pending = Task{};
    slot.has_pending = false;
    slot.in_flight = true;
    return true;
  }

  // Marks the in-flight task complete. Returns true if another task is ready.
  bool Complete(unsigned long analyzer_id) {
    std::lock_guard lock(mutex_);
    auto it = slots_.find(analyzer_id);
    if (it == slots_.end()) {
      return false;
    }
    Slot& slot = it->second;
    slot.in_flight = false;
    if (!slot.backlog.empty()) {
      slot.pending = std::move(slot.backlog.front());
      slot.backlog.pop_front();
      slot.has_pending = true;
      return true;
    }
    return slot.has_pending;
  }

  [[nodiscard]] size_t Size() const {
    std::lock_guard lock(mutex_);
    return slots_.size();
  }

  [[nodiscard]] size_t BufferedTasks() const {
    std::lock_guard lock(mutex_);
    size_t count = 0;
    for (const auto& [id, slot] : slots_) {
      count += slot.backlog.size();
      if (slot.has_pending) {
        ++count;
      }
    }
    return count;
  }

 private:
  struct Slot {
    Task pending;
    bool has_pending{false};
    bool in_flight{false};
    size_t depth{1};
    std::deque<Task> backlog;
  };

  mutable std::mutex mutex_;
  std::unordered_map<unsigned long, Slot> slots_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_DETECTOR_MAILBOX_H
