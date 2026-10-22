#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "Utils/common.h"
#include "alarm.h"
#include "combined_event_manager.h"
#include "scheduler.h"

namespace avs_analyzer {
namespace {

// Alarm ids must be globally unique even when many threads start events at
// the same millisecond.
TEST(AlarmIdTest, ConcurrentIdsAreUnique) {
  constexpr int kThreads = 8;
  constexpr int kIterations = 2000;
  std::vector<std::vector<int64_t>> ids(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&ids, t]() {
      ids[t].reserve(kIterations);
      for (int i = 0; i < kIterations; ++i) {
        ids[t].push_back(NextAlarmId());
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  std::vector<int64_t> all;
  all.reserve(kThreads * kIterations);
  for (const auto& per_thread : ids) {
    all.insert(all.end(), per_thread.begin(), per_thread.end());
  }
  std::sort(all.begin(), all.end());
  EXPECT_GT(all.front(), 0);
  EXPECT_EQ(std::adjacent_find(all.begin(), all.end()), all.end())
      << "duplicate alarm id generated";
}

// Finalizing independent jobs concurrently must not share any mutable state.
TEST(AlarmJobTest, ConcurrentFinalizeIsIndependent) {
  const auto upload_dir =
      std::filesystem::temp_directory_path() / "avs_alarm_job_test";
  auto settings = std::make_shared<const Alarm>(
      upload_dir.string(), /*admin_host=*/"", /*width=*/64, /*height=*/64,
      /*fps=*/25, Alarm::OutputMode::kMp4Only, /*report_delta=*/false);

  constexpr int kThreads = 8;
  constexpr int kIterations = 50;
  std::atomic<int> finalized{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < kIterations; ++i) {
        AlarmJob job;
        job.alarm = settings;
        job.id = NextAlarmId();
        job.video_code = "video-under-test";
        job.start_ts = std::chrono::system_clock::now();
        settings->Finalize(std::move(job));
        finalized.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(finalized.load(), kThreads * kIterations);
}

// An HLS writer that cannot initialize must be inert, not crash.
TEST(HlsEventWriterTest, InvalidBasePathIsInert) {
  Recorder::HlsConfig hls_config;
  HlsEventWriter writer(64, 64, 25, "/proc/avs-does-not-exist/stream",
                        hls_config);
  EXPECT_FALSE(writer.valid());
  EXPECT_FALSE(writer.paths().has_value());

  Event event;
  writer.AddFrames(event);
  EXPECT_FALSE(writer.Close().has_value());
}
}  // namespace
}  // namespace avs_analyzer
