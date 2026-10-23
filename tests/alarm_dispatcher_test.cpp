#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "alarm/alarm_io_dispatcher.h"

namespace avs_analyzer {
namespace {

// A full queue must drop the newest tasks instead of growing without bound.
TEST(AlarmIoDispatcherTest, BoundedQueueDropsNewestAndDrains) {
  constexpr size_t kCapacity = 128;
  constexpr int kTasks = 1000;
  constexpr size_t kThreads = 2;

  AlarmIoDispatcher dispatcher(kCapacity, kThreads);

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate_open = false;
  std::atomic<int> started{0};

  int accepted = 0;
  for (int i = 0; i < kTasks; ++i) {
    const bool ok = dispatcher.TrySubmit([&]() {
      started.fetch_add(1, std::memory_order_relaxed);
      std::unique_lock lock(gate_mutex);
      gate_cv.wait(lock, [&] { return gate_open; });
    });
    if (ok) {
      ++accepted;
    }
  }

  // Consumers may have picked up to kThreads tasks, everything else is queued
  // (at most kCapacity) or counted as dropped.
  EXPECT_LT(accepted, kTasks);
  EXPECT_GT(dispatcher.Dropped(), 0u);
  EXPECT_LE(dispatcher.Depth(), kCapacity);
  EXPECT_EQ(static_cast<uint64_t>(kTasks - accepted), dispatcher.Dropped());

  {
    std::lock_guard lock(gate_mutex);
    gate_open = true;
  }
  gate_cv.notify_all();

  dispatcher.Drain();

  EXPECT_EQ(started.load(std::memory_order_relaxed), accepted);
  EXPECT_EQ(dispatcher.Depth(), 0u);
}

// Submitting after Drain must fail rather than silently losing the task.
TEST(AlarmIoDispatcherTest, RejectsTasksAfterDrain) {
  AlarmIoDispatcher dispatcher(4, 1);
  EXPECT_TRUE(dispatcher.TrySubmit([] {}));
  dispatcher.Drain();
  EXPECT_FALSE(dispatcher.TrySubmit([] {}));
  EXPECT_EQ(dispatcher.Depth(), 0u);
}

// Tasks submitted before Drain are executed even when Drain is called
// immediately.
TEST(AlarmIoDispatcherTest, DrainRunsQueuedTasks) {
  AlarmIoDispatcher dispatcher(32, 1);
  std::atomic<int> ran{0};
  for (int i = 0; i < 32; ++i) {
    ASSERT_TRUE(dispatcher.TrySubmit(
        [&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
  }
  dispatcher.Drain();
  EXPECT_EQ(ran.load(std::memory_order_relaxed), 32);
}

}  // namespace
}  // namespace avs_analyzer
