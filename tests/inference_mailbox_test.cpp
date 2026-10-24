#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <opencv2/core.hpp>

#include "detector/mailbox.h"

namespace {

using avs_analyzer::AnalyzerMailboxes;

AnalyzerMailboxes::ImagePtr MakeImage() {
  return std::make_shared<const cv::Mat>(cv::Mat::zeros(4, 4, CV_8UC3));
}

AnalyzerMailboxes::Task MakeTask(unsigned long analyzer_id, uint64_t ts) {
  return {{analyzer_id, 1, static_cast<unsigned long>(ts)}, MakeImage()};
}

TEST(InferenceMailbox, RejectsUnregisteredAnalyzer) {
  AnalyzerMailboxes boxes;
  const auto outcome = boxes.Push(MakeTask(7, 1));
  EXPECT_EQ(outcome.result, AnalyzerMailboxes::PushResult::kRejected);
  EXPECT_EQ(boxes.Size(), 0u);
}

TEST(InferenceMailbox, LiveDepthKeepsOnlyNewestPending) {
  AnalyzerMailboxes boxes;
  boxes.Add(1, 1);

  auto first = boxes.Push(MakeTask(1, 10));
  EXPECT_EQ(first.result, AnalyzerMailboxes::PushResult::kSchedule);
  EXPECT_EQ(first.buffered_delta, 1);

  AnalyzerMailboxes::Task acquired;
  ASSERT_TRUE(boxes.Acquire(1, acquired));
  EXPECT_EQ(acquired.id[2], 10u);

  const auto second = boxes.Push(MakeTask(1, 20));
  EXPECT_EQ(second.result, AnalyzerMailboxes::PushResult::kQueued);
  EXPECT_EQ(second.buffered_delta, 1);

  // A newer live frame overwrites the pending one; no extra buffering.
  const auto third = boxes.Push(MakeTask(1, 30));
  EXPECT_EQ(third.result, AnalyzerMailboxes::PushResult::kQueued);
  EXPECT_EQ(third.buffered_delta, 0);
  EXPECT_EQ(boxes.BufferedTasks(), 1u);

  EXPECT_TRUE(boxes.Complete(1));
  ASSERT_TRUE(boxes.Acquire(1, acquired));
  EXPECT_EQ(acquired.id[2], 30u);
  EXPECT_EQ(boxes.BufferedTasks(), 0u);
}

TEST(InferenceMailbox, OfflineBacklogIsBoundedAndFifo) {
  constexpr size_t kDepth = 4;
  AnalyzerMailboxes boxes;
  boxes.Add(2, kDepth);

  ASSERT_EQ(boxes.Push(MakeTask(2, 0)).result,
            AnalyzerMailboxes::PushResult::kSchedule);
  AnalyzerMailboxes::Task acquired;
  ASSERT_TRUE(boxes.Acquire(2, acquired));

  for (uint64_t ts = 1; ts <= 20; ++ts) {
    boxes.Push(MakeTask(2, ts));
    EXPECT_LE(boxes.BufferedTasks(), kDepth - 1);
  }

  // Oldest frames are dropped; the retained window is contiguous and FIFO.
  std::vector<uint64_t> seen;
  seen.push_back(acquired.id[2]);
  while (boxes.Complete(2)) {
    ASSERT_TRUE(boxes.Acquire(2, acquired));
    seen.push_back(acquired.id[2]);
    if (seen.size() > kDepth + 1) {
      FAIL() << "backlog exceeded configured depth";
    }
  }

  ASSERT_EQ(seen.size(), kDepth);
  // The retained frames are the newest (depth - 1) ones plus the in-flight
  // frame, in FIFO order.
  for (size_t i = 2; i < seen.size(); ++i) {
    EXPECT_EQ(seen[i], seen[i - 1] + 1);
  }
  EXPECT_EQ(seen[1], 20u - (kDepth - 2));
  EXPECT_EQ(seen.back(), 20u);
}

TEST(InferenceMailbox, RemoveReportsDiscardedTasks) {
  AnalyzerMailboxes boxes;
  boxes.Add(3, 2);

  ASSERT_EQ(boxes.Push(MakeTask(3, 1)).result,
            AnalyzerMailboxes::PushResult::kSchedule);
  AnalyzerMailboxes::Task acquired;
  ASSERT_TRUE(boxes.Acquire(3, acquired));

  boxes.Push(MakeTask(3, 2));
  boxes.Push(MakeTask(3, 3));
  EXPECT_EQ(boxes.BufferedTasks(), 1u);

  EXPECT_EQ(boxes.Remove(3), 1u);
  EXPECT_EQ(boxes.Size(), 0u);
  EXPECT_EQ(boxes.BufferedTasks(), 0u);
}

TEST(InferenceMailbox, AnalyzersAreIndependent) {
  AnalyzerMailboxes boxes;
  boxes.Add(10, 1);
  boxes.Add(11, 1);

  ASSERT_EQ(boxes.Push(MakeTask(10, 100)).result,
            AnalyzerMailboxes::PushResult::kSchedule);
  ASSERT_EQ(boxes.Push(MakeTask(11, 200)).result,
            AnalyzerMailboxes::PushResult::kSchedule);

  AnalyzerMailboxes::Task task;
  ASSERT_TRUE(boxes.Acquire(11, task));
  EXPECT_EQ(task.id[0], 11u);
  EXPECT_EQ(task.id[2], 200u);

  ASSERT_TRUE(boxes.Acquire(10, task));
  EXPECT_EQ(task.id[0], 10u);
  EXPECT_EQ(task.id[2], 100u);
}

TEST(InferenceMailbox, EightAnalyzersCanBeScheduledIndependently) {
  constexpr unsigned long kFirstAnalyzer = 100;
  constexpr int kAnalyzerCount = 8;
  AnalyzerMailboxes boxes;
  for (int i = 0; i < kAnalyzerCount; ++i) {
    boxes.Add(kFirstAnalyzer + static_cast<unsigned long>(i), 1);
  }

  for (int i = 0; i < kAnalyzerCount; ++i) {
    const auto outcome = boxes.Push(MakeTask(
        kFirstAnalyzer + static_cast<unsigned long>(i),
        static_cast<uint64_t>(i + 1)));
    EXPECT_EQ(outcome.result, AnalyzerMailboxes::PushResult::kSchedule);
  }

  std::vector<unsigned long> acquired_ids;
  AnalyzerMailboxes::Task task;
  for (int i = 0; i < kAnalyzerCount; ++i) {
    const auto analyzer_id =
        kFirstAnalyzer + static_cast<unsigned long>(i);
    ASSERT_TRUE(boxes.Acquire(analyzer_id, task));
    acquired_ids.push_back(task.id[0]);
  }

  ASSERT_EQ(acquired_ids.size(), kAnalyzerCount);
  std::sort(acquired_ids.begin(), acquired_ids.end());
  for (int i = 0; i < kAnalyzerCount; ++i) {
    EXPECT_EQ(acquired_ids[static_cast<size_t>(i)],
              kFirstAnalyzer + static_cast<unsigned long>(i));
  }
  EXPECT_EQ(boxes.BufferedTasks(), 0u);
}

}  // namespace

namespace {

TEST(SharedInference, WaitBudgetOutlastsTheClaimersResultTimeout) {
  // Regression: the wait budget used to be a hard-coded 4 s while the
  // analyzer result timeout defaults to 5 s, so a slow claimer made every
  // waiter time out and run the same inference again.
  using namespace std::chrono_literals;
  EXPECT_GT(avs_analyzer::SharedResultWaitFor(5000ms), 5000ms);
  EXPECT_GT(avs_analyzer::SharedResultWaitFor(100ms), 100ms);
  EXPECT_GE(avs_analyzer::SharedResultWaitFor(100ms), 4000ms);
}

TEST(SharedInference, WaitBudgetIsMonotonic) {
  using namespace std::chrono_literals;
  EXPECT_LE(avs_analyzer::SharedResultWaitFor(2000ms),
            avs_analyzer::SharedResultWaitFor(5000ms));
  EXPECT_LE(avs_analyzer::SharedResultWaitFor(5000ms),
            avs_analyzer::SharedResultWaitFor(10000ms));
}

TEST(SharedInference, WaiterReceivesPublishedResult) {
  using namespace std::chrono_literals;
  avs_analyzer::SharedInference shared;
  ASSERT_TRUE(shared.TryClaim());

  DL_RESULT sentinel{};
  sentinel.classId = 42;
  sentinel.confidence = 0.75f;
  sentinel.box = cv::Rect(1, 2, 3, 4);
  std::thread publisher([&shared, sentinel] {
    std::this_thread::sleep_for(20ms);
    shared.Publish({sentinel});
  });

  std::vector<DL_RESULT> out;
  ASSERT_TRUE(
      shared.AwaitResult(out, avs_analyzer::SharedResultWaitFor(1000ms)));
  publisher.join();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].classId, 42);
  EXPECT_FLOAT_EQ(out[0].confidence, 0.75f);
  EXPECT_EQ(out[0].box, cv::Rect(1, 2, 3, 4));
}

TEST(SharedInference, WaiterTimesOutWithoutResult) {
  using namespace std::chrono_literals;
  avs_analyzer::SharedInference shared;
  ASSERT_TRUE(shared.TryClaim());
  std::vector<DL_RESULT> out;
  EXPECT_FALSE(shared.AwaitResult(out, 20ms));
  EXPECT_TRUE(out.empty());
}

}  // namespace
