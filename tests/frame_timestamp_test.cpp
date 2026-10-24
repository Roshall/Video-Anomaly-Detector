// Timestamp resolution and synthesis, including the "new generation
// restarts the synthesised timeline" rule the decode loop relies on.
#include <gtest/gtest.h>

#include "frame.h"
#include "video_processing/helper.h"
#include "stream/frame_timestamp.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {
namespace {

using avs_analyzer::AVFramePtr;

TEST(FrameTimestamp, ValidPtsIsRescaled) {
  AVFramePtr frame{av_frame_alloc()};
  ASSERT_TRUE(frame);
  frame->best_effort_timestamp = 40;  // 1/25 s time base
  frame->pts = 40;
  frame->time_base = AVRational{1, 25};

  int64_t timestamp = 0;
  int64_t timestamp_ms = 0;
  TimestampState state;
  EXPECT_TRUE(ResolveFrameTimestamp(frame.get(), frame->time_base,
                                    AVRational{0, 1}, timestamp, timestamp_ms,
                                    state));
  EXPECT_EQ(timestamp_ms, 1600);
  EXPECT_EQ(state.last_ms, 1600);
}

TEST(FrameTimestamp, PtsFallbackWhenBestEffortMissing) {
  AVFramePtr frame{av_frame_alloc()};
  ASSERT_TRUE(frame);
  frame->best_effort_timestamp = AV_NOPTS_VALUE;
  frame->pts = 25;
  frame->time_base = AVRational{1, 25};

  int64_t timestamp = 0;
  int64_t timestamp_ms = 0;
  TimestampState state;
  EXPECT_TRUE(ResolveFrameTimestamp(frame.get(), frame->time_base,
                                    AVRational{0, 1}, timestamp, timestamp_ms,
                                    state));
  EXPECT_EQ(timestamp_ms, 1000);
}

TEST(FrameTimestamp, MissingPtsIsSynthesisedFromFps) {
  AVFramePtr frame{av_frame_alloc()};
  ASSERT_TRUE(frame);
  frame->time_base = AVRational{1, 1000};
  // The decode loop hands the resolver a freshly unreferenced frame each time,
  // so a written-back timestamp never survives into the next call.
  auto fresh_frame = [&frame] {
    av_frame_unref(frame.get());
    frame->best_effort_timestamp = AV_NOPTS_VALUE;
    frame->pts = AV_NOPTS_VALUE;
  };

  int64_t timestamp = 0;
  int64_t timestamp_ms = 0;
  TimestampState state;
  state.last_ms = 1000;

  fresh_frame();
  EXPECT_TRUE(ResolveFrameTimestamp(frame.get(), frame->time_base,
                                    AVRational{25, 1}, timestamp, timestamp_ms,
                                    state));
  EXPECT_EQ(timestamp_ms, 1040);
  EXPECT_EQ(state.last_ms, 1040);

  fresh_frame();
  EXPECT_TRUE(ResolveFrameTimestamp(frame.get(), frame->time_base,
                                    AVRational{25, 1}, timestamp, timestamp_ms,
                                    state));
  EXPECT_EQ(timestamp_ms, 1080);
}

TEST(FrameTimestamp, FreshGenerationStartsSynthesisedTimelineAtZero) {
  AVFramePtr frame{av_frame_alloc()};
  ASSERT_TRUE(frame);
  frame->best_effort_timestamp = AV_NOPTS_VALUE;
  frame->pts = AV_NOPTS_VALUE;
  frame->time_base = AVRational{1, 1000};

  int64_t timestamp = 0;
  int64_t timestamp_ms = 0;
  TimestampState state;
  state.last_ms = 9000;  // previous generation's last timestamp
  state.last_ms = -1;    // ...but the generation was just reset

  EXPECT_TRUE(ResolveFrameTimestamp(frame.get(), frame->time_base,
                                    AVRational{25, 1}, timestamp, timestamp_ms,
                                    state));
  EXPECT_EQ(timestamp_ms, 0);
}

TEST(FrameTimestamp, MissingPtsWithoutFpsIsRejected) {
  AVFramePtr frame{av_frame_alloc()};
  ASSERT_TRUE(frame);
  frame->best_effort_timestamp = AV_NOPTS_VALUE;
  frame->pts = AV_NOPTS_VALUE;
  frame->time_base = AVRational{1, 1000};

  int64_t timestamp = 0;
  int64_t timestamp_ms = 0;
  TimestampState state;
  EXPECT_FALSE(ResolveFrameTimestamp(frame.get(), frame->time_base,
                                     AVRational{0, 1}, timestamp, timestamp_ms,
                                     state));
  EXPECT_TRUE(state.warned);
  // Still rejected (and still silent) on the next frame.
  EXPECT_FALSE(ResolveFrameTimestamp(frame.get(), frame->time_base,
                                     AVRational{0, 1}, timestamp, timestamp_ms,
                                     state));
}

}  // namespace
}  // namespace avs_analyzer
