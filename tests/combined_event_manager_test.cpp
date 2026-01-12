#include "combined_event_manager.h"
#include <gtest/gtest.h>

extern "C" {
#include <libavutil/frame.h>
}

using namespace avs_analyzer;

static AVFrame* make_frame() {
  AVFrame* f = av_frame_alloc();
  if (!f) return nullptr;
  f->pts = 1;
  f->best_effort_timestamp = 1;
  return f;
}

TEST(CombinedEventManagerTest, StartAndEndEdgeReported) {
  CombinedEventManager mgr;
  CombinedEventConfig cfg{};
  cfg.pre_event_buffer_ms = 0;
  cfg.post_event_buffer_ms = 100; // short buffer for test
  cfg.max_event_duration_ms = 1000;
  mgr.UpdateConfig(cfg);

  AVFramePtr frame{make_frame()};
  ASSERT_TRUE(frame);

  // 1) idle frame: no edges
  auto res = mgr.ProcessFrame(frame.get(), 1000, 1, {});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kNone);

  // 2) a start event -> became_active
  CombinedEventManager::DetectorFrameInfo info{};
  info.monitor_code = "m1";
  info.state = EventDetector::kEventStart;
  info.event_start_timestamp_ms = 900;
  info.event_start_pts = 1;

  res = mgr.ProcessFrame(frame.get(), 1010, 2, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kBecameActive);

  // 3) remain active -> no new active edge
  res = mgr.ProcessFrame(frame.get(), 1020, 3, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kNone);

  // 4) event end frame -> enters Ending (no idle yet)
  info.state = EventDetector::kEventEnd;
  res = mgr.ProcessFrame(frame.get(), 1030, 4, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kNone);

  // 5) after post_event_buffer_ms elapses -> became_idle
  res = mgr.ProcessFrame(frame.get(), 1030 + cfg.post_event_buffer_ms, 5, {});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kBecameIdle);
}
