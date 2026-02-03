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

TEST(CombinedEventManagerTest, MaxDurationReachedTransition) {
  CombinedEventManager mgr;
  CombinedEventConfig cfg{};
  cfg.pre_event_buffer_ms = 0;
  cfg.post_event_buffer_ms = 100;
  cfg.slice_ms = 3000;  // 3s slicing interval
  cfg.max_event_duration_ms = 1000;  // 1s max duration
  mgr.UpdateConfig(cfg);

  AVFramePtr frame{make_frame()};
  ASSERT_TRUE(frame);

  // 1) Start event at timestamp 0
  CombinedEventManager::DetectorFrameInfo info{};
  info.monitor_code = "m1";
  info.state = EventDetector::kEventStart;
  info.event_start_timestamp_ms = 0;
  info.event_start_pts = 1;

  auto res = mgr.ProcessFrame(frame.get(), 10, 1, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kBecameActive);

  // 2) Process frames while active - no transition yet
  res = mgr.ProcessFrame(frame.get(), 500, 2, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kNone);

  // 3) At timestamp 1010 (1010ms - 0ms = 1010ms >= max_event_duration_ms of 1000ms)
  // Should trigger kMaxDurationReached
  res = mgr.ProcessFrame(frame.get(), 1010, 3, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kMaxDurationReached);

  // 4) Continue processing after max duration - should remain active but not report transition again
  res = mgr.ProcessFrame(frame.get(), 1500, 4, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kNone);

  // 5) Event ends -> enters Ending state
  info.state = EventDetector::kEventEnd;
  res = mgr.ProcessFrame(frame.get(), 1600, 5, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kNone);

  // 6) After post_event_buffer_ms -> became_idle and max_duration flag should be reset
  res = mgr.ProcessFrame(frame.get(), 1600 + cfg.post_event_buffer_ms, 6, {});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kBecameIdle);
}

TEST(CombinedEventManagerTest, SlicingUsesSliceMs) {
  CombinedEventManager mgr;
  CombinedEventConfig cfg{};
  cfg.pre_event_buffer_ms = 0;
  cfg.post_event_buffer_ms = 100;
  cfg.slice_ms = 500;  // 500ms slicing interval
  cfg.max_event_duration_ms = 5000;  // 5s max duration (higher than slice_ms)
  mgr.UpdateConfig(cfg);

  AVFramePtr frame{make_frame()};
  ASSERT_TRUE(frame);

  // 1) Start event
  CombinedEventManager::DetectorFrameInfo info{};
  info.monitor_code = "m1";
  info.state = EventDetector::kEventStart;
  info.event_start_timestamp_ms = 0;
  info.event_start_pts = 1;

  auto res = mgr.ProcessFrame(frame.get(), 10, 1, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kBecameActive);

  // 2) Process up to 600ms (600ms - 10ms = 590ms >= slice_ms of 500ms)
  // Should trigger slice (flushed_event)
  res = mgr.ProcessFrame(frame.get(), 600, 2, {info});
  EXPECT_TRUE(res.flushed_event.has_value());
  EXPECT_TRUE(res.flushed_event.value()->more_segments);

  // 3) Continue - should not have triggered max_duration yet
  res = mgr.ProcessFrame(frame.get(), 1100, 3, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kNone);

  // 4) At 5100ms (5100 - 10 = 5090ms >= max_event_duration_ms of 5000ms)
  // Should trigger kMaxDurationReached
  res = mgr.ProcessFrame(frame.get(), 5100, 4, {info});
  EXPECT_EQ(res.transition, CombinedEventManager::Result::Transition::kMaxDurationReached);
}
