// Admission-control and shared-inference semantics for multi-monitor workers.
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "admission.h"
#include "detector/mailbox.h"
#include "task.h"

namespace avs_analyzer {
namespace {

AdmissionEntry Entry(unsigned long id, Task task, uint64_t wanted_ts) {
  return AdmissionEntry{id, task, wanted_ts};
}

TEST(Admission, RejectsReservedIdZero) {
  // Regression: Analyzer::NextId is 1-based; id 0 is reserved as the
  // "no analyzer" value (and was once the detector stop sentinel). It must
  // never be admitted, and it must not be counted as a skipped frame.
  const std::vector<AdmissionEntry> entries{
      Entry(0, Task::kWandering, 0),
      Entry(1, Task::kWandering, 0),
  };
  const auto result = AdmitFrame(entries, 1000);
  EXPECT_EQ(result.skipped, 0u);
  ASSERT_EQ(result.groups.size(), 1u);
  EXPECT_EQ(result.groups[0].analyzer_ids, std::vector<unsigned long>{1});
}

TEST(Admission, SkipsFramesTheDetectorDoesNotWantYet) {
  const std::vector<AdmissionEntry> entries{
      Entry(1, Task::kWandering, 500),
      Entry(2, Task::kSleepWork, 0),
  };
  const auto result = AdmitFrame(entries, 200);
  EXPECT_EQ(result.skipped, 1u);
  ASSERT_EQ(result.groups.size(), 1u);
  EXPECT_EQ(result.groups[0].analyzer_ids,
            std::vector<unsigned long>{2});
  // A single sink has nothing to deduplicate.
  EXPECT_EQ(result.groups[0].shared, nullptr);
}

TEST(Admission, GroupsMonitorsOfTheSameTaskIntoOneRequest) {
  const std::vector<AdmissionEntry> entries{
      Entry(1, Task::kWandering, 0),
      Entry(2, Task::kWandering, 0),
      Entry(3, Task::kWandering, 0),
  };
  const auto result = AdmitFrame(entries, 1000);
  EXPECT_EQ(result.skipped, 0u);
  ASSERT_EQ(result.groups.size(), 1u);
  EXPECT_EQ(result.groups[0].analyzer_ids,
            (std::vector<unsigned long>{1, 2, 3}));
  EXPECT_EQ(result.sinks(), 3u);
  ASSERT_NE(result.groups[0].shared, nullptr);
}

TEST(Admission, KeepsDifferentTasksInSeparateGroups) {
  const std::vector<AdmissionEntry> entries{
      Entry(1, Task::kWandering, 0),
      Entry(2, Task::kSleepWork, 0),
      Entry(3, Task::kWandering, 0),
  };
  const auto result = AdmitFrame(entries, 1000);
  ASSERT_EQ(result.groups.size(), 2u);
  EXPECT_EQ(result.sinks(), 3u);
  EXPECT_EQ(result.groups[0].analyzer_ids,
            (std::vector<unsigned long>{1, 3}));
  EXPECT_EQ(result.groups[1].analyzer_ids,
            (std::vector<unsigned long>{2}));
  EXPECT_NE(result.groups[0].shared, nullptr);
  EXPECT_EQ(result.groups[1].shared, nullptr);
}

TEST(Admission, DropsEntriesWithoutAnalyzerId) {
  const std::vector<AdmissionEntry> entries{Entry(0, Task::kWandering, 0)};
  const auto result = AdmitFrame(entries, 1000);
  EXPECT_EQ(result.skipped, 0u);
  EXPECT_TRUE(result.groups.empty());
}

// Emulates three detector sessions picking up the same frame: exactly one
// runs inference, all three deliver a result for that timestamp.
TEST(SharedInference, ThreeSinksRunOneInferenceAndAllDeliver) {
  const std::vector<AdmissionEntry> entries{
      Entry(1, Task::kWandering, 0),
      Entry(2, Task::kWandering, 0),
      Entry(3, Task::kWandering, 0),
  };
  const auto result = AdmitFrame(entries, 1000);
  ASSERT_EQ(result.groups.size(), 1u);
  const auto shared = result.groups[0].shared;
  ASSERT_NE(shared, nullptr);

  std::atomic<int> inference_count{0};
  std::mutex delivered_mutex;
  std::vector<uint64_t> delivered;
  std::vector<std::thread> threads;
  threads.reserve(3);
  for (int i = 0; i < 3; ++i) {
    threads.emplace_back([&] {
      std::vector<DL_RESULT> results;
      if (shared->TryClaim()) {
        inference_count.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        shared->Publish(std::vector<DL_RESULT>{});
      } else {
        ASSERT_TRUE(shared->AwaitResult(
            results, std::chrono::milliseconds(2000)));
      }
      std::lock_guard lock(delivered_mutex);
      delivered.push_back(1000);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(inference_count.load(), 1);
  EXPECT_EQ(delivered.size(), 3u);
  EXPECT_TRUE(shared->done());
}

TEST(SharedInference, AwaitReportsTimeoutWhenNobodyPublishes) {
  SharedInference shared;
  EXPECT_TRUE(shared.TryClaim());
  std::vector<DL_RESULT> results;
  EXPECT_FALSE(shared.AwaitResult(results, std::chrono::milliseconds(10)));
}

}  // namespace
}  // namespace avs_analyzer
