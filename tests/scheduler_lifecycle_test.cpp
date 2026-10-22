#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config.h"
#include "event.h"
#include "monitor.h"
#include "scheduler.h"
#include "task.h"

namespace avs_analyzer {
namespace {

// The config is intentionally missing: the defaults are enough for these
// tests, and model paths resolve to nothing so detector registration fails
// fast and quietly (it is logged and tolerated by design).
constexpr const char* kMissingConfig = "/nonexistent/analyzer-lifecycle.json";

// A stream URL that is guaranteed to refuse connections immediately.
constexpr const char* kUnreachableUrl = "rtsp://127.0.0.1:1/unreachable";

std::unique_ptr<Monitor> MakeStreamMonitor(const std::string& video_code,
                                           const std::string& code) {
  auto monitor = std::make_unique<Monitor>();
  monitor->code = code;
  monitor->video_code = video_code;
  monitor->source_type = "stream";
  monitor->stream_url = kUnreachableUrl;
  monitor->task = Task::kSleepWork;
  monitor->roi_str = "0.1,0.1,0.9,0.1,0.9,0.9,0.1,0.9";
  monitor->levels = {{1}, {"level-1"}};
  monitor->level_mode = LevelMode::kNumPeople;
  return monitor;
}

using AddBatch =
    std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>>;

struct SchedulerLifecycleTest : ::testing::Test {
  SchedulerLifecycleTest() : config(kMissingConfig), scheduler(config) {
    scheduler.SetState(true);
  }

  Config config;
  Scheduler scheduler;
};

// Concurrent adds that all fail to open the stream must not leave a worker
// behind (and must not create duplicate workers along the way).
TEST_F(SchedulerLifecycleTest, ConcurrentFailedAddsLeaveNoWorker) {
  constexpr int kThreads = 8;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::vector<std::vector<std::pair<int, std::string>>> results(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, t, &results]() {
      AddBatch batch;
      batch.emplace_back(MakeStreamMonitor("video-lifecycle",
                                           "code-" + std::to_string(t) + "-a"),
                         EventDetectorConfig{});
      batch.emplace_back(MakeStreamMonitor("video-lifecycle",
                                           "code-" + std::to_string(t) + "-b"),
                         EventDetectorConfig{});
      scheduler.MonitorBatchAdd(std::move(batch), results[t]);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  for (int t = 0; t < kThreads; ++t) {
    ASSERT_EQ(results[t].size(), 2u) << "thread " << t;
    for (const auto& [code, msg] : results[t]) {
      EXPECT_EQ(code, 0) << "thread " << t << ": " << msg;
      EXPECT_FALSE(msg.empty());
    }
  }

  EXPECT_TRUE(scheduler.GetAllMonitors().empty());
  EXPECT_FALSE(scheduler.ContainsMonitor("video-lifecycle", "code-0-a"));
  EXPECT_FALSE(scheduler.ContainsMonitor("video-lifecycle", "code-7-b"));
}

// Removing a monitor from a video code that never had a worker succeeds
// (legacy semantics: nothing to do).
TEST_F(SchedulerLifecycleTest, RemoveUnknownWorkerSucceeds) {
  std::vector<std::pair<int, std::string>> results;
  scheduler.MonitorBatchRemove({{"video-unknown", "code-x"}}, results);

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].first, 1000);
  EXPECT_FALSE(results[0].second.empty());
}

// Once the service is stopping, adds are rejected without touching the map.
TEST_F(SchedulerLifecycleTest, AddAfterStopIsRejected) {
  scheduler.SetState(false);

  AddBatch batch;
  batch.emplace_back(MakeStreamMonitor("video-stopped", "code-stopped"),
                     EventDetectorConfig{});
  std::vector<std::pair<int, std::string>> results;
  scheduler.MonitorBatchAdd(std::move(batch), results);

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].first, 0);
  EXPECT_EQ(results[0].second, "service is stopping");
  EXPECT_TRUE(scheduler.GetAllMonitors().empty());
  EXPECT_FALSE(scheduler.ContainsMonitor("video-stopped", "code-stopped"));
}
}  // namespace
}  // namespace avs_analyzer
