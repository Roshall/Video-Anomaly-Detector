#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "analyzer.h"
#include "config.h"
#include "monitor.h"

namespace {

using namespace std::chrono_literals;

class ProbeAnalyzer : public avs_analyzer::Analyzer {
 public:
  ProbeAnalyzer(avs_analyzer::Monitor* monitor, const avs_analyzer::Config& cfg)
      : Analyzer(monitor, cfg) {}

  std::vector<cv::Rect> RetrieveDetection() const override {
    const auto det = FetchRawDetection();
    last_frame_ts.store(det.frame_ts, std::memory_order_relaxed);
    return det.bbox;
  }

  void SetROIs(const std::vector<std::vector<cv::Point>>&) override {}

  mutable std::atomic<uint64_t> last_frame_ts{0};
};

avs_analyzer::Monitor MakeMonitor() {
  avs_analyzer::Monitor monitor;
  monitor.task = avs_analyzer::Task::kBreakIn;
  monitor.conf_threshold = 0.5f;
  return monitor;
}

avs_analyzer::Config MakeConfig(int timeout_ms) {
  // Missing file: Config keeps documented defaults, and every field this test
  // depends on is assigned explicitly below.
  avs_analyzer::Config config("analyzer_timeout_test_missing.json");
  config.inference_result_timeout_ms = timeout_ms;
  return config;
}

}  // namespace

TEST(AnalyzerId, IdsAreOneBasedAndUnique) {
  // Regression: the first analyzer in a process used to get id 0, which
  // admission control silently dropped (never submitted to inference).
  auto monitor = MakeMonitor();
  auto config = MakeConfig(1000);
  ProbeAnalyzer first(&monitor, config);
  ProbeAnalyzer second(&monitor, config);
  EXPECT_NE(first.id(), 0u);
  EXPECT_NE(second.id(), 0u);
  EXPECT_NE(first.id(), second.id());
}

TEST(AnalyzerTimeout, ReturnsEmptyDetectionAfterTimeout) {
  auto monitor = MakeMonitor();
  const auto config = MakeConfig(100);
  ProbeAnalyzer analyzer(&monitor, config);
  analyzer.SetNextFrameTimestamp(1234);

  const auto start = std::chrono::steady_clock::now();
  const auto detections = analyzer.RetrieveDetection();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(detections.empty());
  EXPECT_EQ(analyzer.last_frame_ts.load(std::memory_order_relaxed), 1234u);
  EXPECT_GE(elapsed, 80ms);
  EXPECT_LT(elapsed, 2000ms);
}

TEST(AnalyzerTimeout, StopUnblocksWaitingDetection) {
  auto monitor = MakeMonitor();
  const auto config = MakeConfig(60000);
  ProbeAnalyzer analyzer(&monitor, config);
  analyzer.SetNextFrameTimestamp(4321);

  std::atomic<bool> finished{false};
  std::thread worker([&] {
    analyzer.RetrieveDetection();
    finished.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(50ms);
  const auto start = std::chrono::steady_clock::now();
  analyzer.Stop();
  worker.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(finished.load(std::memory_order_acquire));
  EXPECT_LT(elapsed, 1000ms);
}
