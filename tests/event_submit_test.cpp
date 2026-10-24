// Guard contracts for the per-frame submission path (admission happens
// before the convert/clone, so every skip is cheap and counted).
#include <gtest/gtest.h>

#include <memory>

#include "event/event_pipeline.h"
#include "frame.h"
#include "video_processing/helper.h"
#include "metrics.h"
#include "video_processing/format_convertor.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {
using avs_analyzer::AVFramePtr;
using avs_analyzer::ConvertorState;
using avs_analyzer::EventPipeline;
using avs_analyzer::FormatConvertor;
using avs_analyzer::WorkerMetrics;

constexpr int kWidth = 64;
constexpr int kHeight = 48;

AVFramePtr MakeFrame(int width, int height, AVPixelFormat format) {
  AVFramePtr frame{av_frame_alloc()};
  if (!frame) return nullptr;
  frame->format = format;
  frame->width = width;
  frame->height = height;
  if (av_frame_get_buffer(frame.get(), 32) < 0) return nullptr;
  frame->pts = 0;
  frame->best_effort_timestamp = 0;
  return frame;
}

class SubmitFrameTest : public ::testing::Test {
 protected:
  SubmitFrameTest() {
    metrics_ = avs_analyzer::MetricsRegistry::Instance().RegisterWorker(
        "submit-frame-test");
    pipeline_ = std::make_unique<EventPipeline>(nullptr, "submit-frame-test",
                                                1, false, metrics_);
  }
  ~SubmitFrameTest() override {
    pipeline_.reset();
    avs_analyzer::MetricsRegistry::Instance().UnregisterWorker(
        "submit-frame-test");
  }

  std::shared_ptr<WorkerMetrics> metrics_;
  std::unique_ptr<EventPipeline> pipeline_;
};

TEST_F(SubmitFrameTest, NullConvertorCountsSkipped) {
  AVFramePtr frame = MakeFrame(kWidth, kHeight, AV_PIX_FMT_YUV420P);
  ASSERT_TRUE(frame);
  ConvertorState state;  // convertor == nullptr

  pipeline_->SubmitFrame(frame.get(), state, 0, {});
  EXPECT_EQ(metrics_->frames_skipped.load(std::memory_order_relaxed), 1u);
}

TEST_F(SubmitFrameTest, GeometryMismatchCountsSkipped) {
  AVFramePtr frame = MakeFrame(32, 24, AV_PIX_FMT_YUV420P);
  ASSERT_TRUE(frame);
  ConvertorState state;
  state.width = kWidth;
  state.height = kHeight;
  state.format = AV_PIX_FMT_YUV420P;
  state.convertor =
      std::make_shared<FormatConvertor>(kWidth, kHeight, AV_PIX_FMT_YUV420P,
                                        kWidth, kHeight);

  pipeline_->SubmitFrame(frame.get(), state, 0, {});
  EXPECT_EQ(metrics_->frames_skipped.load(std::memory_order_relaxed), 1u);
}

TEST_F(SubmitFrameTest, PixelFormatMismatchCountsSkipped) {
  AVFramePtr frame = MakeFrame(kWidth, kHeight, AV_PIX_FMT_NV12);
  ASSERT_TRUE(frame);
  ConvertorState state;
  state.width = kWidth;
  state.height = kHeight;
  state.format = AV_PIX_FMT_YUV420P;
  state.convertor =
      std::make_shared<FormatConvertor>(kWidth, kHeight, AV_PIX_FMT_YUV420P,
                                        kWidth, kHeight);

  pipeline_->SubmitFrame(frame.get(), state, 0, {});
  EXPECT_EQ(metrics_->frames_skipped.load(std::memory_order_relaxed), 1u);
}

TEST_F(SubmitFrameTest, EmptyMonitorSetIsANoOp) {
  AVFramePtr frame = MakeFrame(kWidth, kHeight, AV_PIX_FMT_YUV420P);
  ASSERT_TRUE(frame);
  ConvertorState state;
  state.width = kWidth;
  state.height = kHeight;
  state.format = AV_PIX_FMT_YUV420P;
  state.convertor =
      std::make_shared<FormatConvertor>(kWidth, kHeight, AV_PIX_FMT_YUV420P,
                                        kWidth, kHeight);

  pipeline_->SubmitFrame(frame.get(), state, 0, {});
  EXPECT_EQ(metrics_->frames_skipped.load(std::memory_order_relaxed), 0u);
  EXPECT_EQ(metrics_->frames_submitted.load(std::memory_order_relaxed), 0u);
}

}  // namespace
