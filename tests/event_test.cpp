//
// Created by lg on 9/28/25.
//
#include "event.h"
#include "frame.h"

#include "gtest/gtest.h"

// Helper function to create frames for tests
avs_analyzer::Frame CreateFrame(double timestamp, bool has_object) {
  avs_analyzer::Frame f;
  f.timestamp = timestamp;
  f.object_count = has_object;
  return f;
}

class EventDetectorTest : public ::testing::Test {
 protected:
  avs_analyzer::EventDetectorConfig config;
  std::unique_ptr<avs_analyzer::EventDetector> detector;

  void SetUp() override {
    // A typical configuration for testing
    config.major_stride_ms = 1.0;
    config.minor_stride_ms = 0.2;
    config.window_size = 5;
    config.min_vote = 3;
    config.pre_event_buffer_ms = 1.0;
    config.post_event_buffer_ms = 1.0;
    config.max_event_duration_ms = 5.0;
    detector = std::make_unique<avs_analyzer::EventDetector>(config);
  }
};

TEST_F(EventDetectorTest, NoObjectsNoEvent) {
  for (int i = 0; i < 50; ++i) {
    auto result = detector->ProcessFrame(CreateFrame(i * 0.2, false));
    ASSERT_FALSE(result.has_value());
  }
  auto result = detector->Flush();
  ASSERT_FALSE(result.has_value());
}

TEST_F(EventDetectorTest, VerificationFails) {
  // This sequence should trigger minor check but fail verification
  detector->ProcessFrame(CreateFrame(1.0, true));  // Enters minor check
  detector->ProcessFrame(CreateFrame(1.2, true));  // vote 2
  detector->ProcessFrame(CreateFrame(1.4, false)); // vote 2
  detector->ProcessFrame(CreateFrame(1.6, false)); // vote 2
  auto result = detector->ProcessFrame(CreateFrame(1.8, false)); // vote 2, window full -> fail

  ASSERT_FALSE(result.has_value());
}

TEST_F(EventDetectorTest, SimpleEventDetectedAndEnds) {
  std::optional<avs_analyzer::Event> final_event;
  // --- Trigger event ---
  detector->ProcessFrame(CreateFrame(1.0, false));
  detector->ProcessFrame(CreateFrame(2.0, true));  // Major check -> switch to Minor
  detector->ProcessFrame(CreateFrame(2.2, false));
  detector->ProcessFrame(CreateFrame(2.401, true));
  detector->ProcessFrame(CreateFrame(2.602, false));
  detector->ProcessFrame(CreateFrame(2.803, true)); // 3rd vote, event confirmed!

  // --- Event active ---
  detector->ProcessFrame(CreateFrame(3.0, true));
  detector->ProcessFrame(CreateFrame(3.2, true));

  // --- Event ends ---
  detector->ProcessFrame(CreateFrame(3.4, false)); // votes: T,T,T,T,F (4)
  detector->ProcessFrame(CreateFrame(3.6, false)); // votes: T,T,T,F,F (3)
  detector->ProcessFrame(CreateFrame(3.8, false)); // votes: T,T,F,F,F (2) -> Event ends
  final_event = detector->ProcessFrame(CreateFrame(4.0, false));
  for (size_t i{1}; i < 6; ++i) {
    final_event = detector->ProcessFrame(CreateFrame(4.0 + i * 0.2, false));
  }

  ASSERT_TRUE(final_event.has_value());
  EXPECT_NEAR(final_event->start_timestamp, 2.0, 0.01);
  EXPECT_NEAR(final_event->end_timestamp, 4.0, 0.01);
  EXPECT_NEAR(final_event->GetDuration(), 2.0, 0.01);

  // Check buffers. Start should be ~1.0s, End should be ~4.8s
  EXPECT_NEAR(final_event->frames.front().timestamp, 2.0, 0.01);
  EXPECT_NEAR(final_event->frames.back().timestamp, 5.0, 0.01);
}


TEST_F(EventDetectorTest, MaxDurationSplitsEvent) {
  std::optional<avs_analyzer::Event> first_event;

  // Trigger event quickly
  detector->ProcessFrame(CreateFrame(1.0, true));
  detector->ProcessFrame(CreateFrame(1.201, true));
  detector->ProcessFrame(CreateFrame(1.402, true));
  detector->ProcessFrame(CreateFrame(1.603, true));
  detector->ProcessFrame(CreateFrame(1.804, true)); // Event confirmed

  // Keep it active for a long time
  double t = 2.05;
  while(t < 8.0) {
    auto result = detector->ProcessFrame(CreateFrame(t, true));
    if (result) {
      first_event = result;
      break;
    }
    t += 0.2;
  }

  ASSERT_TRUE(first_event.has_value());
  // Event start time should be the first frame with an object
  EXPECT_NEAR(first_event->start_timestamp, 1.0, 0.01);
  // Event should last for max_duration
  EXPECT_NEAR(first_event->GetDuration(), config.max_event_duration_ms, 0.2);
}