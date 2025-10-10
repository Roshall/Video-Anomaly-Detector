//
// Created by lg on 9/28/25.
//

#ifndef ANALYZER_EVENT_H
#define ANALYZER_EVENT_H

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "annotator.h"
#include "format_convertor.h"
#include "frame.h"

namespace avs_analyzer {
class Analyzer;

// Represents a detected event, containing all relevant frames.
struct Event {
  int64_t start_timestamp{};
  int64_t end_timestamp{};
  bool more_segments{false};

  std::vector<std::unique_ptr<Frame>> frames{};

  [[nodiscard]] int64_t GetDuration() const {
    return end_timestamp - start_timestamp;
  }
};

// Configuration for the EventDetector.
struct EventDetectorConfig { // all time in base ms
  int64_t major_stride_s{2000};
  int64_t minor_stride_s{200};
  int window_size{5};
  int min_vote{3};
  int64_t pre_event_buffer_s{1000};
  int64_t post_event_buffer_s{1000};
  int64_t max_event_duration_s{60000};
};

// Processes a stream of frames to detect events based on object presence.
class EventDetector {
 public:
  enum State {
    kNoEvent, kEventStart, kEventActive, kEventSplit, kEventEnd,
  };

  EventDetector(const EventDetectorConfig& config, const Analyzer& analyzer,
                FormatConvertor convertor, std::vector<cv::Point>& roi_points,
                AVFrame* sample);

  // Processes a single frame and returns an Event if one has just been completed.
  State ProcessFrame(RawFrame frame);

  [[nodiscard]] std::unique_ptr<Frame> latest_frame();
  int num_detection() const {return static_cast<int>(kept_bboxes_.size());}
  [[nodiscard]] int64_t event_duration() const {
      if (current_state_ != InnerState::kEventActive) {
        return 0;
      } else {
        return frame_buffer_.back()->timestamp - active_event_start_ts_;
      }
  };
  int64_t timestamp() const;

  // Flushes any pending event when the stream ends.
  std::unique_ptr<Event> Flush();

 private:
  enum class InnerState { kMajorCheck, kMinorCheck, kEventActive, kEventEnding };


  std::vector<cv::Rect> Detect(AVFrame* frame);

  // Helper to find the timestamp of the first positive vote in the window.
  std::pair<int64_t, int64_t> FindFirstVoteTimestamp() const;

  const EventDetectorConfig config_;
  InnerState current_state_{InnerState::kMajorCheck};

  VideoOverlay painter_;
  FormatConvertor convertor_;
  const Analyzer& analyzer_;

  int64_t last_major_check_ts_{-2000};  // Initialized to allow first frame check.
  int64_t last_minor_check_ts_{0};

  // Buffer for all recent frames, used to construct events.
  std::deque<std::unique_ptr<Frame>> frame_buffer_;
  // Sliding window to track object presence for voting.
  std::deque<bool> object_votes_window_;
  std::vector<cv::Rect> kept_bboxes_{};
  int64_t last_spilt_time_{-1};
  int64_t active_event_start_ts_{0};
  int64_t max_buffer_time_{0};
  int64_t start_frame_ts_{};
};
}
#endif  // ANALYZER_EVENT_H
