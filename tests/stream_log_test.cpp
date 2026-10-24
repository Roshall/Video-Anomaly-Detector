// Decode-queue overflow logging is rate limited: first drop of a generation
// and then every 100th. Without this a congested stream floods the log.
#include <gtest/gtest.h>

#include "av_pull_stream.h"

TEST(StreamOverflowLogTest, LogsFirstDropAndEveryHundredth) {
  using avs_analyzer::AvPullStream;
  EXPECT_TRUE(AvPullStream::ShouldLogOverflow(1));
  EXPECT_FALSE(AvPullStream::ShouldLogOverflow(2));
  EXPECT_FALSE(AvPullStream::ShouldLogOverflow(99));
  EXPECT_TRUE(AvPullStream::ShouldLogOverflow(100));
  EXPECT_FALSE(AvPullStream::ShouldLogOverflow(101));
  EXPECT_TRUE(AvPullStream::ShouldLogOverflow(200));
  EXPECT_TRUE(AvPullStream::ShouldLogOverflow(300));
}

TEST(StreamOverflowLogTest, NeverLogsBeforeTheFirstDrop) {
  EXPECT_FALSE(avs_analyzer::AvPullStream::ShouldLogOverflow(0));
}
