// EventPipeline lifecycle and guard contracts that do not require inference.
#include <gtest/gtest.h>

#include <memory>

#include "event/event_pipeline.h"
#include "metrics.h"
#include "video_processing/video_context.h"

namespace {

class EventPipelineTest : public ::testing::Test {
 protected:
  EventPipelineTest()
      : metrics_(avs_analyzer::MetricsRegistry::Instance().RegisterWorker(
            "event-pipeline-test")),
        pipeline_(nullptr, "event-pipeline-test", 1, false, metrics_) {}

  ~EventPipelineTest() override {
    avs_analyzer::MetricsRegistry::Instance().UnregisterWorker(
        "event-pipeline-test");
  }

  std::shared_ptr<avs_analyzer::WorkerMetrics> metrics_;
  avs_analyzer::EventPipeline pipeline_;
};

TEST_F(EventPipelineTest, EmptyMonitorConfigCreatesAnIdleManager) {
  pipeline_.UpdateConfig({});

  EXPECT_FALSE(pipeline_.HasActiveEvent());
  EXPECT_EQ(pipeline_.CurrentEventId(), 0);
  EXPECT_TRUE(pipeline_.FlushPending().empty());
}

TEST_F(EventPipelineTest, NullFrameWithEmptyMonitorsIsANoOp) {
  pipeline_.UpdateConfig({});

  const auto result = pipeline_.ProcessFrame(nullptr, 1000, 1000, {});

  EXPECT_FALSE(result.annotated_frame);
  EXPECT_FALSE(result.flushed_event.has_value());
  EXPECT_TRUE(result.alarms.empty());
  EXPECT_EQ(result.event_id, 0);
}

TEST_F(EventPipelineTest, InvalidVideoContextDoesNotPublishAnnotator) {
  VideoContext context;
  context.width = 640;
  context.height = 480;
  context.format = AV_PIX_FMT_NONE;

  pipeline_.EnsureAnnotator(context, {});

  EXPECT_FALSE(pipeline_.HasAnnotator());
}

TEST_F(EventPipelineTest, EmptyMonitorConfigCanBeRefreshed) {
  pipeline_.UpdateConfig({});
  pipeline_.UpdateConfig({}, true);

  EXPECT_FALSE(pipeline_.HasActiveEvent());
  EXPECT_TRUE(pipeline_.FlushPending().empty());
}

}  // namespace
