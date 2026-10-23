#include "alarm_io_dispatcher.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "Utils/log.h"

namespace avs_analyzer {

AlarmIoDispatcher::AlarmIoDispatcher(size_t capacity, size_t thread_count)
    : capacity_(std::max<size_t>(capacity, 1)) {
  const size_t count = std::max<size_t>(thread_count, 1);
  workers_.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    workers_.emplace_back([this] { Consume(); });
  }
}

AlarmIoDispatcher::~AlarmIoDispatcher() { Drain(); }

bool AlarmIoDispatcher::TrySubmit(std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return false;
    }
    if (queue_.size() >= capacity_) {
      ++dropped_;
      // Keep the log rate low under sustained overload.
      if (dropped_ % 100 == 1) {
        LOGW("alarm I/O queue full ({} pending, {} dropped); dropping newest task",
             queue_.size(), dropped_);
      }
      return false;
    }
    queue_.push_back(std::move(task));
  }
  has_work_.notify_one();
  return true;
}

void AlarmIoDispatcher::Consume() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      has_work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      task();
    } catch (const std::exception& e) {
      LOGE("alarm I/O task failed: {}", e.what());
    } catch (...) {
      LOGE("alarm I/O task failed with an unknown exception");
    }
  }
}

void AlarmIoDispatcher::Drain() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  has_work_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

size_t AlarmIoDispatcher::Depth() const {
  std::lock_guard lock(mutex_);
  return queue_.size();
}

uint64_t AlarmIoDispatcher::Dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

}  // namespace avs_analyzer
