#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "combined_event_manager.h"

using namespace avs_analyzer;

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr int64_t kStepMs = 40;  // 25 fps

AVFramePtr MakeFrame(int64_t pts) {
  AVFramePtr frame{av_frame_alloc()};
  if (!frame) return nullptr;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = kWidth;
  frame->height = kHeight;
  if (av_frame_get_buffer(frame.get(), 32) < 0) return nullptr;
  if (av_frame_make_writable(frame.get()) < 0) return nullptr;
  for (int y = 0; y < kHeight; ++y) {
    std::memset(frame->data[0] + y * frame->linesize[0], (y + pts) & 0xFF,
                kWidth);
  }
  for (int y = 0; y < kHeight / 2; ++y) {
    std::memset(frame->data[1] + y * frame->linesize[1], 128, kWidth / 2);
    std::memset(frame->data[2] + y * frame->linesize[2], 128, kWidth / 2);
  }
  frame->pts = pts;
  frame->best_effort_timestamp = pts;
  return frame;
}

CombinedEventConfig MakeConfig() {
  CombinedEventConfig config;
  config.pre_event_buffer_ms = 2000;
  config.post_event_buffer_ms = 1000;
  config.slice_ms = 3000;
  config.guard_window_ms = 1000;
  return config;
}

CombinedEventManager::DetectorFrameInfo ActiveInfo(int64_t event_start_ms,
                                                   int64_t event_start_pts) {
  CombinedEventManager::DetectorFrameInfo info;
  info.monitor_code = "monitor";
  info.state = EventDetector::kEventActive;
  info.detections.emplace_back(1, 1, 8, 8);
  info.event_start_timestamp_ms = event_start_ms;
  info.event_start_pts = event_start_pts;
  return info;
}

CombinedEventManager::DetectorFrameInfo EndInfo() {
  CombinedEventManager::DetectorFrameInfo info;
  info.monitor_code = "monitor";
  info.state = EventDetector::kEventEnd;
  info.detections.emplace_back(1, 1, 8, 8);
  return info;
}

}  // namespace

TEST(EventBufferTest, IdleFramesAreStoredEncodedAndStayWithinBudget) {
  CombinedEventManager manager;
  manager.UpdateConfig(MakeConfig());
  constexpr size_t kBudgetBytes = 8 * 1024;
  manager.UpdateMemoryBudget(kBudgetBytes);

  AVFramePtr source = MakeFrame(0);
  ASSERT_TRUE(source);

  for (int64_t ts = 0; ts <= 30000; ts += kStepMs) {
    source->pts = ts;
    source->best_effort_timestamp = ts;
    const auto result = manager.ProcessFrame(source.get(), ts, ts, {});
    ASSERT_TRUE(result.annotated_frame);
    // The live push pipeline always gets a raw frame, even when the buffer
    // only keeps the encoded copy.
    ASSERT_TRUE(result.annotated_frame->data);
  }

  EXPECT_GT(manager.buffer_frames(), 0u);
  EXPECT_FALSE(manager.has_raw_buffered_frames());
  EXPECT_LE(manager.buffer_bytes(), kBudgetBytes);
  EXPECT_GT(manager.buffer_evictions(), 0u);
}

TEST(EventBufferTest, ActiveEventIsNotTruncatedByByteBudget) {
  CombinedEventManager manager;
  manager.UpdateConfig(MakeConfig());
  // Fits the idle JPEG pre-roll (~100 frames x ~1.5 KB) but not the raw
  // active segment (~125 frames x 6 KB). With state-blind eviction the
  // budget truncated the recorded slice mid-event, dropping the trigger
  // moment itself.
  constexpr size_t kBudgetBytes = 256 * 1024;
  manager.UpdateMemoryBudget(kBudgetBytes);

  AVFramePtr source = MakeFrame(0);
  ASSERT_TRUE(source);

  // Idle phase: the pre-roll fits, so nothing is evicted yet.
  size_t max_bytes = 0;
  for (int64_t ts = 0; ts < 10000; ts += kStepMs) {
    source->pts = ts;
    source->best_effort_timestamp = ts;
    manager.ProcessFrame(source.get(), ts, ts, {});
    max_bytes = std::max(max_bytes, manager.buffer_bytes());
  }
  EXPECT_EQ(manager.buffer_evictions(), 0u);
  EXPECT_LE(max_bytes, kBudgetBytes);

  // Active phase: raw frames push the buffer past the budget, but the
  // active segment must never be evicted.
  const int64_t event_start_ms = 9800;
  std::unique_ptr<Event> slice;
  for (int64_t ts = 10000; ts <= 12800; ts += kStepMs) {
    source->pts = ts;
    source->best_effort_timestamp = ts;
    const auto info = ActiveInfo(event_start_ms, event_start_ms);
    auto result = manager.ProcessFrame(source.get(), ts, ts, {info});
    max_bytes = std::max(max_bytes, manager.buffer_bytes());
    if (result.flushed_event) {
      ASSERT_FALSE(slice);
      slice = std::move(result.flushed_event.value());
    }
  }

  // The budget was genuinely exceeded during the active segment ...
  EXPECT_GT(max_bytes, kBudgetBytes);
  // ... and not a single frame was evicted because of it.
  EXPECT_EQ(manager.buffer_evictions(), 0u);

  // The flushed slice covers the full segment from the pre-roll start.
  ASSERT_TRUE(slice);
  EXPECT_EQ(slice->frames.front()->timestamp, event_start_ms - 2000);
  EXPECT_EQ(slice->frames.back()->timestamp, 12800);
}

TEST(EventBufferTest, IdleBufferDoesNotGrowWithoutBound) {
  CombinedEventManager manager;
  manager.UpdateConfig(MakeConfig());
  manager.UpdateMemoryBudget(256ull * 1024 * 1024);

  AVFramePtr source = MakeFrame(0);
  ASSERT_TRUE(source);

  size_t bytes_after_five_minutes = 0;
  size_t frames_after_five_minutes = 0;
  constexpr int64_t kTenMinutesMs = 10 * 60 * 1000;

  for (int64_t ts = 0; ts <= kTenMinutesMs; ts += kStepMs) {
    source->pts = ts;
    source->best_effort_timestamp = ts;
    manager.ProcessFrame(source.get(), ts, ts, {});
    if (ts == 5 * 60 * 1000) {
      bytes_after_five_minutes = manager.buffer_bytes();
      frames_after_five_minutes = manager.buffer_frames();
    }
  }

  // The idle window is pre + guard + safety = 4000 ms => ~101 frames.
  EXPECT_LE(manager.buffer_frames(), 110u);
  EXPECT_NEAR(static_cast<double>(manager.buffer_frames()),
              static_cast<double>(frames_after_five_minutes), 2.0);
  EXPECT_NEAR(static_cast<double>(manager.buffer_bytes()),
              static_cast<double>(bytes_after_five_minutes),
              static_cast<double>(bytes_after_five_minutes) * 0.05 + 1024);
  EXPECT_FALSE(manager.has_raw_buffered_frames());
}

TEST(EventBufferTest, PreRollIsMaterialisedWithOriginalTimestamps) {
  CombinedEventManager manager;
  manager.UpdateConfig(MakeConfig());
  manager.UpdateMemoryBudget(256ull * 1024 * 1024);

  AVFramePtr source = MakeFrame(0);
  ASSERT_TRUE(source);

  // Idle first so the pre-roll is buffered as JPEG.
  for (int64_t ts = 0; ts < 10000; ts += kStepMs) {
    source->pts = ts;
    source->best_effort_timestamp = ts;
    manager.ProcessFrame(source.get(), ts, ts, {});
  }
  EXPECT_FALSE(manager.has_raw_buffered_frames());

  // Event starts at 10000 ms but the detector reports it began at 9800 ms.
  const int64_t event_start_ms = 9800;
  std::unique_ptr<Event> slice;
  for (int64_t ts = 10000; ts <= 12800; ts += kStepMs) {
    source->pts = ts;
    source->best_effort_timestamp = ts;
    const auto info = ActiveInfo(event_start_ms, event_start_ms);
    auto result = manager.ProcessFrame(source.get(), ts, ts, {info});
    if (result.flushed_event) {
      ASSERT_FALSE(slice);
      slice = std::move(result.flushed_event.value());
    }
  }

  ASSERT_TRUE(slice);
  EXPECT_TRUE(slice->more_segments);
  ASSERT_FALSE(slice->frames.empty());

  // Pre-roll starts at event_start - pre_event_buffer_ms.
  EXPECT_EQ(slice->frames.front()->timestamp, event_start_ms - 2000);
  EXPECT_EQ(slice->frames.back()->timestamp, 12800);
  for (const auto& frame : slice->frames) {
    ASSERT_TRUE(frame);
    ASSERT_TRUE(frame->data) << "encoded frame was not materialised";
    EXPECT_EQ(frame->data->best_effort_timestamp, frame->timestamp);
    EXPECT_EQ(frame->data->width, kWidth);
    EXPECT_EQ(frame->data->height, kHeight);
    EXPECT_EQ(frame->data->format, AV_PIX_FMT_YUV420P);
  }

  // The buffer is empty after the slice flush.
  EXPECT_EQ(manager.buffer_frames(), 0u);
  EXPECT_EQ(manager.buffer_bytes(), 0u);

  // End the event and verify the post-roll is included up to the final flush.
  // End of event is reported at 12840 ms; with a 1000 ms post-roll the final
  // flush happens at 13840 ms.
  std::unique_ptr<Event> final_event;
  for (int64_t ts = 12840; ts <= 13000; ts += kStepMs) {
    source->pts = ts;
    source->best_effort_timestamp = ts;
    auto result = manager.ProcessFrame(source.get(), ts, ts, {EndInfo()});
    if (result.flushed_event) {
      final_event = std::move(result.flushed_event.value());
    }
  }
  for (int64_t ts = 13040; ts <= 13840; ts += kStepMs) {
    source->pts = ts;
    source->best_effort_timestamp = ts;
    auto result = manager.ProcessFrame(source.get(), ts, ts, {});
    if (result.flushed_event) {
      final_event = std::move(result.flushed_event.value());
    }
  }

  ASSERT_TRUE(final_event);
  EXPECT_FALSE(final_event->more_segments);
  ASSERT_FALSE(final_event->frames.empty());
  EXPECT_EQ(final_event->frames.back()->timestamp, 13840);
  EXPECT_TRUE(final_event->frames.back()->data);
}
