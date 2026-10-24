#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "config.h"
#include "detector/mailbox.h"

namespace {

class ConfigTest : public ::testing::Test {
 protected:
  void TearDown() override {
    for (const auto& path : files_) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }

  std::string WriteConfig(const std::string& body) {
    const auto dir = std::filesystem::temp_directory_path();
    const auto path =
        dir / ("avs_config_test_" + std::to_string(seq_++) + ".json");
    const auto upload = dir / ("avs_config_test_upload_" + std::to_string(seq_));
    std::filesystem::create_directories(upload);
    std::ofstream out(path, std::ios::binary);
    EXPECT_TRUE(out.is_open());
    // uploadDir is required by Config; point it at a scratch directory so the
    // test neither logs an error nor touches the real upload path.
    const std::string inner =
        body.size() >= 2 ? body.substr(1, body.size() - 2) : "";
    out << "{\"uploadDir\":\"" << upload.string() << "\"";
    if (!inner.empty()) {
      out << "," << inner;
    }
    out << "}";
    out.close();
    files_.push_back(path);
    files_.push_back(upload.string());
    return path.string();
  }

  int seq_{0};
  std::vector<std::string> files_;
};

}  // namespace

TEST_F(ConfigTest, EventBufferJpegQualityIsParsed) {
  const auto path =
      WriteConfig(R"({"performance":{"eventBufferJpegQuality":15}})");
  avs_analyzer::Config config(path.c_str());
  EXPECT_EQ(config.event_buffer_jpeg_quality, 15);
}

TEST_F(ConfigTest, EventBufferJpegQualityDefaultsToCurrentBehaviour) {
  const auto path = WriteConfig("{}");
  avs_analyzer::Config config(path.c_str());
  EXPECT_EQ(config.event_buffer_jpeg_quality, 8);
}

TEST_F(ConfigTest, EventBufferJpegQualityIsClampedToJpegRange) {
  const auto too_low =
      WriteConfig(R"({"performance":{"eventBufferJpegQuality":1}})");
  avs_analyzer::Config low(too_low.c_str());
  EXPECT_EQ(low.event_buffer_jpeg_quality, 2);

  const auto too_high =
      WriteConfig(R"({"performance":{"eventBufferJpegQuality":99}})");
  avs_analyzer::Config high(too_high.c_str());
  EXPECT_EQ(high.event_buffer_jpeg_quality, 31);
}

TEST_F(ConfigTest, EmptyPreloadModelsDisablesPreloading) {
  const auto path = WriteConfig(R"({"performance":{"preloadModels":[]}})");
  avs_analyzer::Config config(path.c_str());
  EXPECT_TRUE(config.preload_models.empty());
}

TEST_F(ConfigTest, MissingPreloadModelsKeepsTheDefaults) {
  const auto path = WriteConfig("{}");
  avs_analyzer::Config config(path.c_str());
  EXPECT_FALSE(config.preload_models.empty());
}

TEST_F(ConfigTest, SharedResultWaitOutlastsTheDefaultResultTimeout) {
  // Locks the pair of defaults: a waiter must never give up before the
  // claimer's own result timeout, or both sessions run the same inference.
  const auto path = WriteConfig("{}");
  avs_analyzer::Config config(path.c_str());
  const auto timeout =
      std::chrono::milliseconds(config.inference_result_timeout_ms);
  EXPECT_GT(avs_analyzer::SharedResultWaitFor(timeout), timeout);
}
