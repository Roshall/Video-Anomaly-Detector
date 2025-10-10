//
// Created by lg on 9/28/25.
//
#include "event.h"

#include <algorithm>
#include <iostream>
#include <numeric>

#include "Utils/log.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <analyzer.h>

namespace avs_analyzer {

EventDetector::EventDetector(const EventDetectorConfig& config,
                             const Analyzer& analyzer,
                             FormatConvertor convertor,
                             std::vector<cv::Point>& roi_points, AVFrame* sample)
    : config_(config), painter_{roi_points, sample}, convertor_(std::move(convertor)), analyzer_(analyzer) {
  last_major_check_ts_ = -config_.major_stride_s;
  max_buffer_time_ = config_.pre_event_buffer_s +
                     (config_.minor_stride_s * config_.window_size) +
                     config_.post_event_buffer_s +
                     1000;
}

std::pair<int64_t, int64_t> EventDetector::FindFirstVoteTimestamp() const {
  auto it = std::find(object_votes_window_.cbegin(),
                      object_votes_window_.cend(), true);
  const std::unique_ptr<Frame>* frame;
  if (it != object_votes_window_.cend()) {
    auto index = std::distance(object_votes_window_.cbegin(), it);
    // The frame_buffer_ contains more frames than the window, so we need to find the correct starting point.
    // The last frame in the buffer corresponds to the last vote in the window.
    if (frame_buffer_.size() > (object_votes_window_.size() - 1 - index)) {
      frame = &frame_buffer_[frame_buffer_.size() - object_votes_window_.size() +index];
    }
  }
  frame = &frame_buffer_.back();
  return {(*frame)->timestamp, (*frame)->data->best_effort_timestamp};
}

EventDetector::State EventDetector::ProcessFrame(RawFrame frame) {
  State state{State::kNoEvent};
  switch (current_state_) {
    case InnerState::kMajorCheck: {
      if (frame.timestamp - last_major_check_ts_ >= config_.major_stride_s) {
        last_major_check_ts_ = frame.timestamp;
        auto bboxes = Detect(frame.data);
        if (not bboxes.empty()) {
          std::cout << "[Major] Object found at t=" << frame.timestamp
                    << "s. Switching to MINOR_CHECK." << std::endl;
          current_state_ = InnerState::kMinorCheck;
          last_minor_check_ts_ = frame.timestamp;
          object_votes_window_.push_back(true);
          kept_bboxes_ = std::move(bboxes);
        }
      }
      break;
    }

    case InnerState::kMinorCheck: {
      if (frame.timestamp - last_minor_check_ts_ >= config_.minor_stride_s) {
        last_minor_check_ts_ = frame.timestamp;

        auto bboxes = Detect(frame.data);
        object_votes_window_.push_back(not bboxes.empty());
        if (not bboxes.empty()) {
          kept_bboxes_ = std::move(bboxes);
        }

        if (object_votes_window_.size() == config_.window_size) {
          int current_votes = std::accumulate(object_votes_window_.begin(),
                                              object_votes_window_.end(), 0);
          if (current_votes >= config_.min_vote) {
            std::cout << ">>> EVENT DETECTED at t=" << frame.timestamp
                      << "s <<<" << std::endl;
            current_state_ = InnerState::kEventActive;
            state = State::kEventStart;
            // TODO: add cover page for the event
            std::tie(active_event_start_ts_, start_frame_ts_) = FindFirstVoteTimestamp();
          } else {
            std::cout << "[Minor] Verification failed. Votes: " << current_votes
                      << ". Back to MAJOR_CHECK." << std::endl;
            current_state_ = InnerState::kMajorCheck;
            kept_bboxes_.clear();
            object_votes_window_.clear();
          }
        }
      }
      break;
    }

    case InnerState::kEventActive: {
      if (frame.timestamp - last_minor_check_ts_ >= config_.minor_stride_s) {
        last_minor_check_ts_ = frame.timestamp;
        auto bboxes = Detect(frame.data);
        object_votes_window_.push_back(not bboxes.empty());
        if (not bboxes.empty()) {
          kept_bboxes_ = std::move(bboxes);
        }

        if (object_votes_window_.size() > config_.window_size) {
          object_votes_window_.pop_front();
        }

        int current_votes = std::accumulate(object_votes_window_.begin(),
                                            object_votes_window_.end(), 0);

        state = State::kEventActive;
        // Check for event end condition
        if (current_votes < config_.min_vote) {
          std::cout << "[Active] Event ending at t=" << frame.timestamp
                    << "s. Votes dropped to " << current_votes << "."
                    << std::endl;
          // Prepare for next event search
          current_state_ = InnerState::kEventEnding;
        }
      }

      // Check for max duration
      int64_t start = last_spilt_time_ < 0 ? active_event_start_ts_: last_spilt_time_;
      if (frame.timestamp - start >=
          config_.max_event_duration_s) {
        std::cout << "[Active] Event reached max duration at t="
                  << frame.timestamp << "s. Saving and splitting." << std::endl;
        state = State::kEventSplit;
        last_spilt_time_ = frame.timestamp;
      }
      break;
    }
    case InnerState::kEventEnding: {
      int64_t start = last_spilt_time_ < 0 ? active_event_start_ts_: last_spilt_time_;
      if (frame.timestamp - start >=
          config_.post_event_buffer_s) {
        // Finalize the event.
        current_state_ = InnerState::kMajorCheck;
        last_major_check_ts_ = frame.timestamp;
        object_votes_window_.clear();
        state = State::kEventEnd;
        last_spilt_time_ = -1;
      }
      break;
    }
  }

  // create output frame
  AVFramePtr out_frame;
  if (not kept_bboxes_.empty()) {
    out_frame = painter_.draw_elements(frame.data, kept_bboxes_, 4);
    av_frame_unref(frame.data);
  } else {
    out_frame.reset(av_frame_alloc());
    av_frame_move_ref(out_frame.get(), frame.data);
  }
  frame_buffer_.emplace_back(std::make_unique<Frame>(frame.timestamp, std::move(out_frame)));
  // Keep buffer size manageable. Max duration + buffers is a safe upper limit.
  if (frame_buffer_.size() > 10 &&
         frame.timestamp - frame_buffer_.front()->timestamp > max_buffer_time_) {
    frame_buffer_.pop_front();
  }

  return state;
}

std::unique_ptr<Event> EventDetector::Flush() {
  if ((current_state_ == InnerState::kMajorCheck||
       current_state_ == InnerState::kEventActive)
      && !frame_buffer_.empty()) {
    auto event = std::make_unique<Event>(start_frame_ts_, frame_buffer_.back()->data->best_effort_timestamp);
    // Find the start index for pre-buffering.
    auto it_start = std::upper_bound(
        frame_buffer_.begin(), frame_buffer_.end(),
        active_event_start_ts_ - config_.pre_event_buffer_s,
        [](int64_t ts, const std::unique_ptr<Frame>& f) { return ts < f->timestamp; });
    event->frames.reserve(std::distance(it_start, frame_buffer_.end()));
    for (;it_start < frame_buffer_.end(); ++it_start) {
      event->frames.emplace_back(std::move(*it_start));
    }
    event->more_segments = current_state_ == InnerState::kEventActive;
    std::cout << "[Saved] Event package ends at t="
              << event->frames.back()->timestamp << std::endl;
    frame_buffer_.clear();
    return event;
  }
  return nullptr;
}
std::vector<cv::Rect> EventDetector::Detect(AVFrame* frame) {
  return analyzer_.ProcessVideoFrame(convertor_(frame));
}
std::unique_ptr<Frame> EventDetector::latest_frame() {
  auto& frame = frame_buffer_.back();
  return std::make_unique<Frame>(frame->timestamp, AVFramePtr{av_frame_clone(frame->data.get())});
}
}