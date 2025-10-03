//
// Created by lg on 9/28/25.
//

#ifndef ANALYZER_EVENT_H
#define ANALYZER_EVENT_H

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "frame.h"

namespace avs_analyzer {
// Represents a detected event, containing all relevant frames.
struct Event {
  double start_timestamp{};
  double end_timestamp{};
  bool more_segments{false};

  std::vector<Frame> frames;

  [[nodiscard]] double GetDuration() const {
    return end_timestamp - start_timestamp;
  }
};

// Configuration for the EventDetector.
struct EventDetectorConfig {
  double major_stride_s{2.0};
  double minor_stride_s{0.2};
  int window_size{5};
  int min_vote{3};
  double pre_event_buffer_s{1.0};
  double post_event_buffer_s{1.0};
  double max_event_duration_s{10.0};
};

// Processes a stream of frames to detect events based on object presence.
class EventDetector {
 public:
  explicit EventDetector(const EventDetectorConfig& config);

  // Processes a single frame and returns an Event if one has just been completed.
  std::optional<Event> ProcessFrame(const Frame& frame);

  // Flushes any pending event when the stream ends.
  std::optional<Event> Flush();

 private:
  enum class State { kMajorCheck, kMinorCheck, kEventActive, kEventEnding };

  // Helper method to finalize and create an event object.
  Event FinalizeEvent(double event_end_timestamp);

  // Helper to find the timestamp of the first positive vote in the window.
  double FindFirstVoteTimestamp() const;

  const EventDetectorConfig config_;
  State current_state_{State::kMajorCheck};

  double last_major_check_ts_{-2.0};  // Initialized to allow first frame check.
  double last_minor_check_ts_{0.0};

  // Buffer for all recent frames, used to construct events.
  std::deque<Frame> frame_buffer_;
  // Sliding window to track object presence for voting.
  std::deque<bool> object_votes_window_;

  double active_event_start_ts_{0.0};
  double active_event_end_ts_{0.0};
};
}
#endif  // ANALYZER_EVENT_H
