//
// Created by lg on 9/28/25.
//

#ifndef ANALYZER_EVENT_H
#define ANALYZER_EVENT_H

#include <deque>
#include <optional>
#include <vector>
#include <opencv2/opencv.hpp>
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
struct EventDetectorConfig {  // all time in base ms
  int64_t major_stride_ms{2000};
  int64_t minor_stride_ms{250};
  int window_size{5};  // not time but count
  int min_vote{3};     // not time but count
  int64_t max_event_duration_ms{60000};
  int64_t pre_event_buffer_ms{1000};
  int64_t post_event_buffer_ms{1000};
  int64_t latest_duration_ms{};
  int minimum_num{1};
  [[nodiscard]] bool defer_start(int num_obj) const;
};

// Processes a stream of frames to detect events based on object presence.
class EventDetector {
 public:
  enum State {
    kNoEvent,
    kEventStart,
    kEventActive,
    kEventEnd,
  };

  EventDetector(const EventDetectorConfig& config, const Analyzer& analyzer);

  // Processes a single frame and returns an Event if one has just been
  // completed.
  EventDetector::State ProcessFrame(const int64_t current_ts, const int64_t best_effort_ts);

  [[nodiscard]] const std::vector<cv::Rect>& detections() const {
    return kept_bboxes_;
  }
  [[nodiscard]] int64_t num_detection() const {
    return static_cast<int>(kept_bboxes_.size());
  }
  [[nodiscard]] int64_t event_duration() const {
    return last_timestamp_ms_ > active_event_start_ts_
               ? last_timestamp_ms_ - active_event_start_ts_
               : 0;
  };
  [[nodiscard]] int64_t timestamp() const;
  [[nodiscard]] int64_t start_ts() const;  // returns best-effort timestamp
  [[nodiscard]] int64_t event_start_timestamp() const {
    return active_event_start_ts_;
  }

 private:
  enum class InnerState {
    kMajorCheck,
    kMinorCheck,
    kDoubleCheck,
    kEventActive
  };
  class VotesWindow {
   public:
    explicit VotesWindow(size_t max_i) : max_item_(max_i) {}
    // return whether window is full
    void slide(bool item, int64_t timestamp, int64_t best_effort_ts) {
      samples_.push_back({item, timestamp, best_effort_ts});
      votes_ += item;
      if (samples_.size() > max_item_) {
        votes_ -= samples_.front().positive;
        samples_.pop_front();
      }
    }
    [[nodiscard]] bool full() const { return samples_.size() == max_item_; }
    void clear() {
      samples_.clear();
      votes_ = 0;
    }
    [[nodiscard]] int votes() const { return votes_; }
    [[nodiscard]] std::optional<std::pair<int64_t, int64_t>> first_positive_timestamp() const;

   private:
    struct Sample {
      bool positive{false};
      int64_t timestamp{0};
      int64_t best_effort_timestamp{0};
    };
    size_t max_item_;
    std::deque<Sample> samples_{};
    int votes_{0};
  };

  std::vector<cv::Rect> Detect() const;

  // Helper to find the timestamp of the first positive vote in the window.
  [[nodiscard]] std::pair<int64_t, int64_t> FindFirstVoteTimestamp() const;

  const EventDetectorConfig config_;
  InnerState current_state_{InnerState::kMajorCheck};

  const Analyzer& analyzer_;

  int64_t last_major_check_ts_{-2000};
  int64_t last_minor_check_ts_{0};
  // Sliding window to track object presence for voting.
  VotesWindow object_votes_window_;
  std::vector<cv::Rect> kept_bboxes_{};
  int64_t active_event_start_ts_{0};
  int64_t active_event_start_pts_{0};
  int64_t last_timestamp_ms_{0};
  int64_t last_best_effort_ts_{0};
};
}  // namespace avs_analyzer
#endif  // ANALYZER_EVENT_H
