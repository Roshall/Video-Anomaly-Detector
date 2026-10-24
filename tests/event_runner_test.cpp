// EventRunner guard contracts that do not need a real frame pipeline.
#include <gtest/gtest.h>

#include <memory>

#include "alarm/alarm_pipeline.h"
#include "event/event_pipeline.h"
#include "event/event_runner.h"
#include "frame_walk_time.h"
#include "metrics.h"

namespace {
using avs_analyzer::AlarmPipeline;
using avs_analyzer::EventPipeline;
using avs_analyzer::EventRunner;

class EventRunnerTest : public ::testing::Test {
 protected:
  EventRunnerTest() {
    metrics_ = avs_analyzer::MetricsRegistry::Instance().RegisterWorker(
        "event-runner-test");
    // Both pipelines tolerate a null scheduler in their constructors.
    events_ = std::make_unique<EventPipeline>(nullptr, "event-runner-test", 1,
                                              false, metrics_);
    avs_analyzer::FrameWallClock timer;
    alarms_ = std::make_unique<AlarmPipeline>(nullptr, "event-runner-test",
                                              false, timer);
  }
  ~EventRunnerTest() override {
    alarms_.reset();
    events_.reset();
    avs_analyzer::MetricsRegistry::Instance().UnregisterWorker(
        "event-runner-test");
  }

  std::shared_ptr<avs_analyzer::WorkerMetrics> metrics_;
  std::unique_ptr<EventPipeline> events_;
  std::unique_ptr<AlarmPipeline> alarms_;
};

TEST_F(EventRunnerTest, SubmitEventIgnoresNullEvents) {
  EventRunner runner(*events_, *alarms_);
  runner.SubmitEvent(nullptr, 7);  // must not crash or enqueue anything
}

TEST_F(EventRunnerTest, ConstructsWithoutHooks) {
  // A runner without hooks must be constructible (hooks are optional).
  EventRunner runner(*events_, *alarms_);
  runner.SubmitEvent(nullptr, 1);
}

}  // namespace
