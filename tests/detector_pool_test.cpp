// DetectorPool single-flight model loading.
//
// Concurrent first registrations of the same (model, target) must load the
// model exactly once, and a failed load must not leave a stale in-flight
// entry that blocks later registrations.
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Utils/log.h"
#include "config.h"
#include "detector/constant_yolo.h"
#include "task.h"

namespace {

namespace fs = std::filesystem;

// The repo keeps models/ untracked, so model-backed tests skip instead of
// failing on a clean checkout.
fs::path FindModelDir() {
  fs::path dir = fs::current_path();
  for (int i = 0; i < 4 && !dir.empty(); ++i, dir = dir.parent_path()) {
    const auto candidate = dir / "models";
    if (fs::exists(candidate / "yolov8n.onnx")) {
      return candidate;
    }
  }
  return {};
}

std::string WriteConfig(const std::string& model_dir,
                        const std::string& preload = "[]") {
  const auto path =
      fs::temp_directory_path() / "avs_detector_pool_test.json";
  std::ofstream out(path, std::ios::binary);
  out << "{\n"
         "  \"uploadDir\": \"/tmp/avs_detector_pool_test_upload\",\n"
         "  \"DLModel\": {\"dir\": \"" +
             model_dir +
         "\", \"acceleratorInference\": false},\n"
         "  \"performance\": {\"preloadModels\": " +
             preload +
         ", \"inferenceWorkersPerModel\": 2},\n"
         "  \"application\": {\"inferenceResultTimeoutMs\": 5000}\n"
         "}\n";
  out.close();
  return path.string();
}

avs_analyzer::YOLODetector::Callback MakeCallback() {
  return {[](const avs_analyzer::YOLODetector::ResultType&, uint64_t) {},
          [] { return std::numeric_limits<uint64_t>::max(); }};
}

}  // namespace

TEST(DetectorPoolTest, ConcurrentRegistrationsLoadTheModelOnce) {
#ifdef ANALYZER_TSAN
  GTEST_SKIP() << "ONNX Runtime is not TSan-instrumented; "
                  "run this test in a normal build";
#endif
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "models/yolov8n.onnx not available";
  }

  const auto config_path = WriteConfig(model_dir.string());
  avs_analyzer::Config config(config_path.c_str());
  ASSERT_TRUE(config.state);

  // LOGI writes through spdlog's default logger; install the project logger
  // first (as main() does) so the sink below sees the load lines.
  spdlog::set_default_logger(
      avs_analyzer::LoggerSingleton::instance().get_logger());

  // Capture the "Loaded model" lines emitted while registrations race.
  std::ostringstream captured;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured);
  auto logger = spdlog::default_logger();
  const size_t sink_index = logger->sinks().size();
  logger->sinks().push_back(sink);

  avs_analyzer::DetectorPool pool(config);

  constexpr int kThreads = 4;
  std::vector<std::future<bool>> futures;
  for (int i = 0; i < kThreads; ++i) {
    futures.push_back(std::async(std::launch::async, [&pool, i] {
      auto add = pool.RegisterToDetector(avs_analyzer::Task::kWandering,
                                         100 + i, MakeCallback());
      add(100 + i, 1, 0, nullptr, nullptr);
      return true;
    }));
  }
  for (auto& future : futures) {
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)),
              std::future_status::ready);
    EXPECT_TRUE(future.get());
  }

  logger->sinks().erase(logger->sinks().begin() + sink_index);

  // Single-flight: one runtime, two sessions -> exactly two load lines.
  // The log prints the full model path.
  const std::string marker =
      "Loaded model " + (model_dir / "yolov8n.onnx").string();
  const std::string log = captured.str();
  size_t load_lines = 0;
  for (size_t pos = log.find(marker); pos != std::string::npos;
       pos = log.find(marker, pos + 1)) {
    ++load_lines;
  }
  EXPECT_EQ(load_lines, 2u) << log;

  const auto status = pool.GetModelStatus();
  ASSERT_EQ(status.size(), 1u);
  EXPECT_EQ(status[0]["sessions"], 2);
  EXPECT_EQ(status[0]["sharedResultWaitMs"],
            avs_analyzer::SharedResultWaitFor(
                std::chrono::milliseconds(config.inference_result_timeout_ms))
                .count());

  std::error_code ec;
  fs::remove(config_path, ec);
}

TEST(DetectorPoolTest, FailedLoadsDoNotBlockLaterRegistrations) {
  const auto config_path =
      WriteConfig("/nonexistent/avs-model-dir", "[\"nonexistent_model.onnx\"]");
  avs_analyzer::Config config(config_path.c_str());
  ASSERT_TRUE(config.state);
  avs_analyzer::DetectorPool pool(config);

  constexpr int kThreads = 4;
  std::vector<std::future<bool>> futures;
  for (int i = 0; i < kThreads; ++i) {
    futures.push_back(std::async(std::launch::async, [&pool, i] {
      auto add = pool.RegisterToDetector(avs_analyzer::Task::kSleepWork,
                                         100 + i, MakeCallback());
      // A failed load degrades to a no-op adder.
      add(100 + i, 1, 0, nullptr, nullptr);
      return true;
    }));
  }
  for (auto& future : futures) {
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)),
              std::future_status::ready)
        << "registration blocked on a stale in-flight load";
    EXPECT_TRUE(future.get());
  }

  // A later registration must also return promptly.
  auto late = std::async(std::launch::async, [&pool] {
    auto add = pool.RegisterToDetector(avs_analyzer::Task::kSleepWork, 999,
                                       MakeCallback());
    add(999, 1, 0, nullptr, nullptr);
    return true;
  });
  EXPECT_EQ(late.wait_for(std::chrono::seconds(10)),
            std::future_status::ready);

  std::error_code ec;
  fs::remove(config_path, ec);
}
