// The decode loop's error-budget / backoff / reconnect state machine.
#include <gtest/gtest.h>

#include <algorithm>

#include "stream/decode_loop_policy.h"

namespace avs_analyzer {
namespace {

TEST(DecodeLoopPolicy, OkResetsTheErrorStreak) {
  DecodeLoopPolicy policy;
  EXPECT_EQ(policy.consecutive_errors(), 0);
  EXPECT_EQ(policy.backoff_ms(), 0);

  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(policy.OnResult(GrabResult::kDecodeError),
              DecodeLoopPolicy::Action::kBackoff);
  }
  EXPECT_EQ(policy.consecutive_errors(), 5);

  EXPECT_EQ(policy.OnResult(GrabResult::kOk), DecodeLoopPolicy::Action::kProcess);
  EXPECT_EQ(policy.consecutive_errors(), 0);
  EXPECT_EQ(policy.backoff_ms(), 0);
}

TEST(DecodeLoopPolicy, BackoffGrowsLinearlyAndNeverExceedsTheCap) {
  DecodeLoopPolicy policy;
  // Backoff is min(kMaxBackoffMs, kBackoffStepMs * errors); with the default
  // budget of 50 errors the largest value a kBackoff action can carry is 490.
  for (int i = 1; i < DecodeLoopPolicy::kMaxConsecutiveErrors; ++i) {
    ASSERT_EQ(policy.OnResult(GrabResult::kAgain),
              DecodeLoopPolicy::Action::kBackoff);
    EXPECT_EQ(policy.backoff_ms(),
              std::min(DecodeLoopPolicy::kMaxBackoffMs,
                       DecodeLoopPolicy::kBackoffStepMs * i));
    EXPECT_LE(policy.backoff_ms(), DecodeLoopPolicy::kMaxBackoffMs);
  }
}

TEST(DecodeLoopPolicy, ErrorBudgetExhaustionRemovesTheWorker) {
  DecodeLoopPolicy policy;
  for (int i = 0; i < DecodeLoopPolicy::kMaxConsecutiveErrors - 1; ++i) {
    ASSERT_EQ(policy.OnResult(GrabResult::kDecodeError),
              DecodeLoopPolicy::Action::kBackoff);
  }
  EXPECT_EQ(policy.OnResult(GrabResult::kDecodeError),
            DecodeLoopPolicy::Action::kRemoveWorker);
}

TEST(DecodeLoopPolicy, ReconnectWaitsWithoutConsumingTheBudget) {
  DecodeLoopPolicy policy;
  EXPECT_EQ(policy.OnResult(GrabResult::kReconnecting),
            DecodeLoopPolicy::Action::kWaitReconnect);
  EXPECT_EQ(policy.consecutive_errors(), 0);
}

TEST(DecodeLoopPolicy, EofAndFatalStopTheLoop) {
  DecodeLoopPolicy policy;
  EXPECT_EQ(policy.OnResult(GrabResult::kEof),
            DecodeLoopPolicy::Action::kStop);
  EXPECT_EQ(policy.OnResult(GrabResult::kFatal),
            DecodeLoopPolicy::Action::kStop);
}

}  // namespace
}  // namespace avs_analyzer
