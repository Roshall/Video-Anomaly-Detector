//
// Created by lg on 9/28/25.
//
#include "event.h"

#include <algorithm>
#include <iostream>
#include <numeric>

EventDetector::EventDetector(const EventDetectorConfig& config) : config_(config) {
  last_major_check_ts_ = -config_.major_stride_s;
}

double EventDetector::FindFirstVoteTimestamp() const {
  auto it = std::find(object_votes_window_.cbegin(), object_votes_window_.cend(), true);
  if (it != object_votes_window_.cend()) {
    auto index = std::distance(object_votes_window_.cbegin(), it);
    // The frame_buffer_ contains more frames than the window, so we need to find the correct starting point.
    // The last frame in the buffer corresponds to the last vote in the window.
    if (frame_buffer_.size() > (object_votes_window_.size() - 1 - index)) {
      return frame_buffer_[frame_buffer_.size() - object_votes_window_.size() + index].timestamp;
    }
  }
  return frame_buffer_.back().timestamp;
}

Event EventDetector::FinalizeEvent(double event_end_timestamp) {
  Event event;
  event.start_timestamp = active_event_start_ts_;
  event.end_timestamp = event_end_timestamp;

  // Find the start index for pre-buffering.
  auto it_start = std::lower_bound(frame_buffer_.begin(), frame_buffer_.end(),
                                   event.start_timestamp - config_.pre_event_buffer_s,
                                   [](const Frame& f, double ts) { return f.timestamp < ts; });

  event.frames.assign(it_start, frame_buffer_.end());
  std::cout << "[Saved] Event package ends at t=" << event.frames.back().timestamp << std::endl;

  // Reset for the next event.
  object_votes_window_.clear();

  return event;
}

std::optional<Event> EventDetector::ProcessFrame(const Frame& frame) {
  frame_buffer_.push_back(frame);
  // Keep buffer size manageable. Max duration + buffers is a safe upper limit.
  double max_buffer_time = config_.pre_event_buffer_s + config_.minor_stride_s * config_.window_size + 1.0;
  while (!frame_buffer_.empty() && frame.timestamp - frame_buffer_.front().timestamp > max_buffer_time) {
    frame_buffer_.pop_front();
  }

  switch (current_state_) {
    case State::kMajorCheck: {
      if (frame.timestamp - last_major_check_ts_ >= config_.major_stride_s) {
        last_major_check_ts_ = frame.timestamp;
        if (frame.has_object) {
          std::cout << "[Major] Object found at t=" << frame.timestamp << "s. Switching to MINOR_CHECK." << std::endl;
          current_state_ = State::kMinorCheck;
          last_minor_check_ts_ = frame.timestamp;
          object_votes_window_.push_back(true);
        }
      }
      break;
    }

    case State::kMinorCheck: {
      if (frame.timestamp - last_minor_check_ts_ >= config_.minor_stride_s) {
        last_minor_check_ts_ = frame.timestamp;
        object_votes_window_.push_back(frame.has_object);

        if (object_votes_window_.size() == config_.window_size) {
          int current_votes = std::accumulate(object_votes_window_.begin(), object_votes_window_.end(), 0);
          if (current_votes >= config_.min_vote) {
            std::cout << ">>> EVENT DETECTED at t=" << frame.timestamp << "s <<<" << std::endl;
            current_state_ = State::kEventActive;
            active_event_start_ts_ = FindFirstVoteTimestamp();
          } else {
            std::cout << "[Minor] Verification failed. Votes: " << current_votes << ". Back to MAJOR_CHECK." << std::endl;
            current_state_ = State::kMajorCheck;
            object_votes_window_.clear();
          }
        }
      }
      break;
    }

    case State::kEventActive: {
      if (frame.timestamp - last_minor_check_ts_ >= config_.minor_stride_s) {
        last_minor_check_ts_ = frame.timestamp;
        object_votes_window_.push_back(frame.has_object);

        if (object_votes_window_.size() > config_.window_size) {
          object_votes_window_.pop_front();
        }

        int current_votes = std::accumulate(object_votes_window_.begin(), object_votes_window_.end(), 0);

        // Check for event end condition
        if (current_votes < config_.min_vote) {
          std::cout << "[Active] Event ending at t=" << frame.timestamp << "s. Votes dropped to " << current_votes << "." << std::endl;
          current_state_ = State::kEventEnding; // Prepare for next event search
          active_event_end_ts_ = frame.timestamp;
        }
      }

      // Check for max duration
      if (frame.timestamp - active_event_start_ts_ >= config_.max_event_duration_s) {
        std::cout << "[Active] Event reached max duration at t=" << frame.timestamp << "s. Saving and splitting." << std::endl;
        current_state_ = State::kMinorCheck; // Immediately look for a new event
        return FinalizeEvent(frame.timestamp);
      }
      break;
    }
    case State::kEventEnding: {
      if (frame.timestamp - active_event_end_ts_ >= config_.post_event_buffer_s) {
        // Finalize the event.
        FinalizeEvent(frame.timestamp);
        current_state_ = State::kMajorCheck;
        last_major_check_ts_ = frame.timestamp;
        object_votes_window_.clear();
        return FinalizeEvent(active_event_end_ts_);
      }
      break;
    }
  }
  return std::nullopt;
}

std::optional<Event> EventDetector::Flush() {
  if (current_state_ == State::kEventActive && !frame_buffer_.empty()) {
    std::cout << "[Flush] Stream ended. Finalizing active event." << std::endl;
    current_state_ = State::kMajorCheck;
    return FinalizeEvent(frame_buffer_.back().timestamp);
  }
  return std::nullopt;
}