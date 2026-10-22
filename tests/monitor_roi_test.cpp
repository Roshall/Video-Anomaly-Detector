#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "monitor.h"
#include "task.h"

using avs_analyzer::Monitor;
using avs_analyzer::Task;

namespace {

// Build a Monitor the way ApiAddMonitor (api_monitor.cpp) does after it has
// extracted "recognitionRegion" from the control JSON object.
Monitor MakeMonitor(Task task, std::string roi_str) {
  Monitor m;
  m.code = "ctrl-001";
  m.video_code = "camera-001";
  m.source_type = "stream";
  m.stream_url = "rtsp://127.0.0.1:554/live";
  m.task = task;
  m.roi_str = std::move(roi_str);
  return m;
}

// Polygon helpers (normalized coordinates, x,y pairs).
constexpr const char* kQuadA = "0.1000,0.2000,0.3000,0.4000,0.5000,0.6000,0.7000,0.8000";
constexpr const char* kQuadB = "0.0500,0.0500,0.9500,0.0500,0.9500,0.9500,0.0500,0.9500";

// Mirrors the recognitionRegion extraction in api_monitor.cpp ApiAddMonitor:
// string -> used as-is, array -> .dump(), anything else -> left empty.
std::string ExtractRoiStrLikeApiMonitor(const nlohmann::json& ctrl) {
  std::string roi_str;
  if (ctrl.contains("recognitionRegion")) {
    const auto& rr = ctrl["recognitionRegion"];
    if (rr.is_string()) {
      roi_str = rr.get<std::string>();
    } else if (rr.is_array()) {
      roi_str = rr.dump();
    }
  }
  return roi_str;
}

}  // namespace

// ---------------------------------------------------------------------------
// Fix #2 (report finding #2): single-polygon string format still works.
// ---------------------------------------------------------------------------
TEST(MonitorRoiTest, SinglePolygonStringParses) {
  auto m = MakeMonitor(Task::kPhonePlaying, kQuadA);
  ASSERT_TRUE(m.ParseRecognitionRegion(1000, 1000));

  ASSERT_EQ(m.roi_points.size(), 4u);
  ASSERT_EQ(m.roi_polygons.size(), 1u);
  ASSERT_EQ(m.roi_polygons[0].size(), 4u);
  EXPECT_EQ(m.roi_points[0], cv::Point(100, 200));
  EXPECT_EQ(m.roi_points[1], cv::Point(300, 400));
  EXPECT_EQ(m.roi_points[2], cv::Point(500, 600));
  EXPECT_EQ(m.roi_points[3], cv::Point(700, 800));
}

// ---------------------------------------------------------------------------
// Fix #2: kBunkOff with the JSON-array dump produced by api_monitor.cpp
// (["0.1,0.2,...","0.3,0.4,..."]) must parse into multiple polygons.
// This is the regression test for the old type_error.302 crash path.
// ---------------------------------------------------------------------------
TEST(MonitorRoiTest, BunkOffJsonArrayDumpParsesMultiplePolygons) {
  nlohmann::json ctrl = nlohmann::json::object();
  ctrl["recognitionRegion"] = nlohmann::json::array({kQuadA, kQuadB});

  // Exactly what api_monitor.cpp does with an array-valued recognitionRegion.
  const std::string roi_str = ExtractRoiStrLikeApiMonitor(ctrl);
  ASSERT_FALSE(roi_str.empty());
  ASSERT_EQ(roi_str.front(), '[');

  auto m = MakeMonitor(Task::kBunkOff, roi_str);
  ASSERT_TRUE(m.ParseRecognitionRegion(1000, 1000)) << "roi_str=" << roi_str;

  ASSERT_EQ(m.roi_polygons.size(), 2u) << "expected 2 polygons, got " << m.roi_polygons.size();
  ASSERT_EQ(m.roi_polygons[0].size(), 4u);
  ASSERT_EQ(m.roi_polygons[1].size(), 4u);

  EXPECT_EQ(m.roi_polygons[0][0], cv::Point(100, 200));
  EXPECT_EQ(m.roi_polygons[0][3], cv::Point(700, 800));
  EXPECT_EQ(m.roi_polygons[1][0], cv::Point(50, 50));
  EXPECT_EQ(m.roi_polygons[1][2], cv::Point(950, 950));

  // Documented behavior: the multi-polygon kBunkOff path fills roi_polygons
  // only; roi_points stays empty.
  EXPECT_TRUE(m.roi_points.empty());
}

// Single-element array must still yield exactly one polygon.
TEST(MonitorRoiTest, BunkOffJsonArrayWithOnePolygon) {
  nlohmann::json ctrl = nlohmann::json::object();
  ctrl["recognitionRegion"] = nlohmann::json::array({kQuadA});
  const std::string roi_str = ExtractRoiStrLikeApiMonitor(ctrl);

  auto m = MakeMonitor(Task::kBunkOff, roi_str);
  ASSERT_TRUE(m.ParseRecognitionRegion(1000, 1000));
  ASSERT_EQ(m.roi_polygons.size(), 1u);
  ASSERT_EQ(m.roi_polygons[0].size(), 4u);
}

// kBunkOff with a plain single-polygon string (not an array) falls back to
// the single-polygon path and fills both roi_points and roi_polygons.
TEST(MonitorRoiTest, BunkOffPlainStringFallsBackToSinglePolygon) {
  auto m = MakeMonitor(Task::kBunkOff, kQuadA);
  ASSERT_TRUE(m.ParseRecognitionRegion(1000, 1000));
  ASSERT_EQ(m.roi_polygons.size(), 1u);
  ASSERT_EQ(m.roi_points.size(), 4u);
  EXPECT_EQ(m.roi_points[0], cv::Point(100, 200));
}

// ---------------------------------------------------------------------------
// Malformed / invalid inputs must fail cleanly (return false, no exception).
// ---------------------------------------------------------------------------
TEST(MonitorRoiTest, BunkOffMalformedJsonArrayFails) {
  auto m = MakeMonitor(Task::kBunkOff, "[\"0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8\"");  // missing ']'
  EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000));
  EXPECT_TRUE(m.roi_polygons.empty());
}

TEST(MonitorRoiTest, BunkOffArrayWithNonStringEntryFails) {
  auto m = MakeMonitor(Task::kBunkOff, "[\"0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8\", 42]");
  EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000));
}

TEST(MonitorRoiTest, BunkOffArrayWithObjectEntryFails) {
  auto m = MakeMonitor(Task::kBunkOff, "[{\"points\":\"0.1,0.2\"}]");
  EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000));
}

TEST(MonitorRoiTest, BunkOffEmptyArrayFails) {
  auto m = MakeMonitor(Task::kBunkOff, "[]");
  EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000));
  EXPECT_TRUE(m.roi_polygons.empty());
}

TEST(MonitorRoiTest, PolygonWithTooFewPointsFails) {
  // 3 points (6 coordinates) -> point_count <= 3 -> invalid.
  auto m1 = MakeMonitor(Task::kPhonePlaying, "0.1,0.2,0.3,0.4,0.5,0.6");
  EXPECT_FALSE(m1.ParseRecognitionRegion(1000, 1000));

  auto m2 = MakeMonitor(Task::kPhonePlaying, "0.1,0.2,0.3");
  EXPECT_FALSE(m2.ParseRecognitionRegion(1000, 1000));

  // Same rule inside a kBunkOff array entry.
  auto m3 = MakeMonitor(Task::kBunkOff, "[\"0.1,0.2,0.3,0.4,0.5,0.6\"]");
  EXPECT_FALSE(m3.ParseRecognitionRegion(1000, 1000));
}

TEST(MonitorRoiTest, EmptyRoiStrFails) {
  auto m1 = MakeMonitor(Task::kPhonePlaying, "");
  EXPECT_FALSE(m1.ParseRecognitionRegion(1000, 1000));

  auto m2 = MakeMonitor(Task::kBunkOff, "");
  EXPECT_FALSE(m2.ParseRecognitionRegion(1000, 1000));

  auto m3 = MakeMonitor(Task::kBunkOff, "   ");  // whitespace only
  EXPECT_FALSE(m3.ParseRecognitionRegion(1000, 1000));
}

// ---------------------------------------------------------------------------
// Documented behavior: the JSON-array format is only honored for kBunkOff.
// For any other task the array text falls through to the single-polygon
// parser, whose first token ("["0.1...") is not numeric -> parse failure.
// The old code crashed here with json type_error.302; it must not throw now.
// ---------------------------------------------------------------------------
TEST(MonitorRoiTest, ArrayFormatOnNonBunkOffTaskFailsCleanly) {
  nlohmann::json ctrl = nlohmann::json::object();
  ctrl["recognitionRegion"] = nlohmann::json::array({kQuadA, kQuadB});
  const std::string roi_str = ExtractRoiStrLikeApiMonitor(ctrl);

  for (Task task : {Task::kPhonePlaying, Task::kSleepWork, Task::kWandering,
                    Task::kBreakIn, Task::kWallClimbing}) {
    auto m = MakeMonitor(task, roi_str);
    EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000))
        << "task=" << avs_analyzer::toString(task) << " roi_str=" << roi_str;
    EXPECT_TRUE(m.roi_points.empty());
    EXPECT_TRUE(m.roi_polygons.empty());
  }
}

// Non-string, non-array recognitionRegion (number/object/null) leaves roi_str
// empty at the api_monitor.cpp level; ParseRecognitionRegion then fails.
TEST(MonitorRoiTest, NonStringNonArrayRegionYieldsEmptyRoiStr) {
  for (const nlohmann::json& bad : {nlohmann::json(42), nlohmann::json(nullptr),
                                    nlohmann::json::object({{"x", 1}})}) {
    nlohmann::json ctrl = nlohmann::json::object();
    ctrl["recognitionRegion"] = bad;
    const std::string roi_str = ExtractRoiStrLikeApiMonitor(ctrl);
    EXPECT_TRUE(roi_str.empty());

    auto m = MakeMonitor(Task::kBunkOff, roi_str);
    EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000));
  }
}

// ---------------------------------------------------------------------------
// Edge cases.
// ---------------------------------------------------------------------------
TEST(MonitorRoiTest, NonPositiveVideoDimensionsFail) {
  auto m1 = MakeMonitor(Task::kPhonePlaying, kQuadA);
  EXPECT_FALSE(m1.ParseRecognitionRegion(0, 1000));
  auto m2 = MakeMonitor(Task::kPhonePlaying, kQuadA);
  EXPECT_FALSE(m2.ParseRecognitionRegion(1000, 0));
  auto m3 = MakeMonitor(Task::kPhonePlaying, kQuadA);
  EXPECT_FALSE(m3.ParseRecognitionRegion(-100, -100));
}

TEST(MonitorRoiTest, OddCoordinateCountFails) {
  auto m = MakeMonitor(Task::kPhonePlaying, "0.1,0.2,0.3");  // 3 tokens
  EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000));
}

TEST(MonitorRoiTest, NonNumericCoordinatesFail) {
  auto m = MakeMonitor(Task::kPhonePlaying, "abc,0.2,0.3,0.4,0.5,0.6,0.7,0.8");
  EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000));
}

TEST(MonitorRoiTest, CoordinatesAreScaledToVideoSize) {
  auto m = MakeMonitor(Task::kBunkOff, "[\"0.5,0.25,0.5,0.75,0.75,0.75,0.75,0.25\"]");
  ASSERT_TRUE(m.ParseRecognitionRegion(1920, 1080));
  ASSERT_EQ(m.roi_polygons.size(), 1u);
  EXPECT_EQ(m.roi_polygons[0][0], cv::Point(960, 270));
  EXPECT_EQ(m.roi_polygons[0][1], cv::Point(960, 810));
}

TEST(MonitorRoiTest, RepeatedParseClearsPreviousState) {
  auto m = MakeMonitor(Task::kBunkOff, "[\"0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8\",\"0.05,0.05,0.95,0.05,0.95,0.95,0.05,0.95\"]");
  ASSERT_TRUE(m.ParseRecognitionRegion(1000, 1000));
  ASSERT_EQ(m.roi_polygons.size(), 2u);

  // A failed re-parse must not leave stale polygons behind.
  m.roi_str = "garbage";
  EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000));
  EXPECT_TRUE(m.roi_points.empty());
  EXPECT_TRUE(m.roi_polygons.empty());
}

TEST(MonitorRoiTest, FailedArrayParseDoesNotLeakPartialPolygonsToSuccess) {
  // First entry valid, second entry invalid: overall result must be false.
  auto m = MakeMonitor(Task::kBunkOff, "[\"0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8\",\"0.1,0.2\"]");
  EXPECT_FALSE(m.ParseRecognitionRegion(1000, 1000));
}
