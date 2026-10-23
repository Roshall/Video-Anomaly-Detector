#ifndef ANALYZER_ALARM_IO_DISPATCHER_H
#define ANALYZER_ALARM_IO_DISPATCHER_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace avs_analyzer {

// Bounded executor for alarm side effects (JPEG write + admin POST, final
// alarm reporting). A slow admin service must not grow memory without bound,
// so when the queue is full the newest task is dropped and counted.
class AlarmIoDispatcher {
 public:
  AlarmIoDispatcher(size_t capacity, size_t thread_count);
  ~AlarmIoDispatcher();
  AlarmIoDispatcher(const AlarmIoDispatcher&) = delete;
  AlarmIoDispatcher& operator=(const AlarmIoDispatcher&) = delete;

  // Returns false when the task was dropped because the queue was full or the
  // dispatcher is shutting down.
  bool TrySubmit(std::function<void()> task);

  // Stops accepting tasks, runs every task already queued, and joins the
  // consumer threads. Idempotent.
  void Drain();

  [[nodiscard]] size_t Depth() const;
  [[nodiscard]] uint64_t Dropped() const;

 private:
  void Consume();

  mutable std::mutex mutex_;
  std::condition_variable has_work_;
  std::deque<std::function<void()>> queue_;
  const size_t capacity_;
  bool stopping_{false};
  uint64_t dropped_{0};
  std::vector<std::thread> workers_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_ALARM_IO_DISPATCHER_H
