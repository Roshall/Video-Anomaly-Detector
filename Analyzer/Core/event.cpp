#include "event.h"

#include <algorithm>
#include <iostream>
#include <numeric>

#include "Utils/log.h"
#include "format_convertor.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <analyzer.h>

namespace avs_analyzer {

EventDetector::EventDetector(const EventDetectorConfig& config,
                             const Analyzer& analyzer,
                             FormatConvertor convertor,
                             const std::vector<cv::Point>& roi_points,
                             AVFrame* sample)
    : config_(config),
      painter_{roi_points, sample},
      convertor_(std::move(convertor)),
      analyzer_(analyzer),
      object_votes_window_(config.window_size) {
  last_major_check_ts_ = -config_.major_stride_ms;
  max_buffer_time_ =
      config_.pre_event_buffer_ms +
      std::max(std::max(config.max_event_duration_ms,
                        (config_.minor_stride_ms * config_.window_size)),
               config.latest_duration_ms) +
      config_.post_event_buffer_ms + 1000;
}

std::pair<int64_t, int64_t> EventDetector::FindFirstVoteTimestamp() const {
  const Frame* frame{};
  if (const auto pos_r = object_votes_window_.first_votes_reserve_pos(); pos_r > 0) {
    // The frame_buffer_ contains more frames than the window, so we need to
    // find the correct starting point. The last frame in the buffer corresponds
    // to the last vote in the window.
    if (frame_buffer_.size() >= pos_r) {
      frame = frame_buffer_[frame_buffer_.size() - pos_r].get();
    }
  }
  if (!frame) {
    frame = frame_buffer_.back().get();
  }
  return {frame->timestamp, frame->data->best_effort_timestamp};
}

EventDetector::State EventDetector::ProcessFrame(const RawFrame frame) {
  State state{State::kNoEvent};
  auto current_ts = frame.timestamp;
  switch (current_state_) {
    case InnerState::kMajorCheck: {
      if (current_ts - last_major_check_ts_ >= config_.major_stride_ms) {
        last_major_check_ts_ = current_ts;
        if (auto bboxes = Detect(frame.data); not bboxes.empty()) {
          LOGD("[Major] Object found at t = {}s. Switching to MINOR_CHECK.",
               current_ts);
          current_state_ = InnerState::kMinorCheck;
          last_minor_check_ts_ = current_ts;
          object_votes_window_.slide(true);
          kept_bboxes_ = std::move(bboxes);
        }
      }
      break;
    }

    case InnerState::kMinorCheck: {
      if (current_ts - last_minor_check_ts_ >= config_.minor_stride_ms) {
        last_minor_check_ts_ = current_ts;
        auto bboxes = Detect(frame.data);
        const auto has_obj = not bboxes.empty();
        if (has_obj) {
          kept_bboxes_ = std::move(bboxes);
        }
        object_votes_window_.slide(has_obj);
        if (object_votes_window_.full()) {
          if (auto votes = object_votes_window_.votes();
              votes >= config_.min_vote) {
            std::tie(active_event_start_ts_, start_frame_ts_) =
                FindFirstVoteTimestamp();
            if (config_.defer_start(static_cast<int>(kept_bboxes_.size()))) {
              current_state_ = InnerState::kDoubleCheck;
              LOGD(">>> Verification succeeded at t = {}ms, Starting Duration Checking", current_ts);
            } else {
              LOGD(">>> EVENT DETECTED at t = {}ms <<<", current_ts);
              current_state_ = InnerState::kEventActive;
              state = State::kEventStart;
            }
          } else {
            LOGD("[Minor] Verification failed. Votes: {}. Back to MAJOR_CHECK.",
                 votes);
            current_state_ = InnerState::kMajorCheck;
            kept_bboxes_.clear();
            object_votes_window_.clear();
          }
        }
      }
      break;
    }
    case InnerState::kDoubleCheck: {
      if (current_ts - last_minor_check_ts_ >= config_.minor_stride_ms) {
        last_minor_check_ts_ = current_ts;
        auto bboxes = Detect(frame.data);
        const auto has_obj = not bboxes.empty();
        object_votes_window_.slide(has_obj);
        if (has_obj) {
          kept_bboxes_ = std::move(bboxes);
        }

        if (auto votes = object_votes_window_.votes();
            votes >= config_.min_vote) {
          if (current_ts - active_event_start_ts_ >= config_.latest_duration_ms && kept_bboxes_.size() >= config_.minimum_num) {
            LOGD(">>> EVENT DETECTED at t = {}ms <<<", current_ts);
            current_state_ = InnerState::kEventActive;
            state = kEventStart;
          }
        } else {
          LOGD(
              "[Duration Check] Verification failed. Votes: {}. Back to "
              "MAJOR_CHECK.",
              votes);
          current_state_ = InnerState::kMajorCheck;
          kept_bboxes_.clear();
          object_votes_window_.clear();
        }
      }
      break;
    }

    case InnerState::kEventActive: {
      if (current_ts - last_minor_check_ts_ >= config_.minor_stride_ms) {
        last_minor_check_ts_ = current_ts;
        auto bboxes = Detect(frame.data);
        const auto has_obj = not bboxes.empty();
        object_votes_window_.slide(has_obj);
        if (has_obj) {
          kept_bboxes_ = std::move(bboxes);
        }

        state = kEventActive;
        // Check for event end condition
        if (auto votes = object_votes_window_.votes();
            votes < config_.min_vote || kept_bboxes_.size() < config_.minimum_num) {
          LOGD("[Active] Event ending at t = {}ms. Votes dropped to {}.",
               current_ts, votes);
          // Prepare for next event search
          current_state_ = InnerState::kEventEnding;
        }
      }

      // Check for max duration
      const int64_t start =
          last_spilt_time_ < 0 ? active_event_start_ts_ : last_spilt_time_;
      if (current_ts - start >= config_.max_event_duration_ms) {
        LOGI(
            "[Active] Event reached max duration at t = {}ms. Saving and "
            "splitting.",
            current_ts);
        state = State::kEventSplit;
        last_spilt_time_ = current_ts;
      }
      break;
    }
    case InnerState::kEventEnding: {
      const int64_t start =
          last_spilt_time_ < 0 ? active_event_start_ts_ : last_spilt_time_;
      if (current_ts - start >= config_.post_event_buffer_ms) {
        // Finalize the event.
        current_state_ = kept_bboxes_.empty() ? InnerState::kMajorCheck: InnerState::kDoubleCheck;
        last_major_check_ts_ = current_ts;
        object_votes_window_.clear();
        kept_bboxes_.clear();
        state = kEventEnd;
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
  frame_buffer_.emplace_back(
      std::make_unique<Frame>(current_ts, std::move(out_frame)));
  // Keep buffer size manageable. Max duration + buffers is a safe upper limit.
  if (frame_buffer_.size() > 10 &&
      current_ts - frame_buffer_.front()->timestamp > max_buffer_time_) {
    frame_buffer_.pop_front();
  }

  return state;
}

std::unique_ptr<Event> EventDetector::Flush() {
  if ((current_state_ == InnerState::kMajorCheck ||
       current_state_ == InnerState::kEventActive) &&
      !frame_buffer_.empty()) {
    auto event = std::make_unique<Event>(
        start_frame_ts_, frame_buffer_.back()->data->best_effort_timestamp);
    // Find the start index for pre-buffering.
    auto it_start =
        std::upper_bound(frame_buffer_.begin(), frame_buffer_.end(),
                         active_event_start_ts_ - config_.pre_event_buffer_ms,
                         [](const int64_t ts, const std::unique_ptr<Frame>& f) {
                           return ts < f->timestamp;
                         });
    event->frames.reserve(std::distance(it_start, frame_buffer_.end()));
    for (; it_start < frame_buffer_.end(); ++it_start) {
      event->frames.emplace_back(std::move(*it_start));
    }
    event->more_segments = current_state_ == InnerState::kEventActive;
    LOGI("[Saved] Event package ends at t = {}ms",
         event->frames.back()->timestamp);
    frame_buffer_.clear();
    return event;
  }
  return nullptr;
}
std::vector<cv::Rect> EventDetector::Detect(AVFrame* frame) {
  return analyzer_.ProcessVideoFrame(convertor_(frame));
}
std::unique_ptr<Frame> EventDetector::latest_frame() const {
  const auto& frame = frame_buffer_.back();
  return std::make_unique<Frame>(frame->timestamp,
                                 AVFramePtr{av_frame_clone(frame->data.get())});
}
int64_t EventDetector::timestamp() const {
  return frame_buffer_.back()->data->best_effort_timestamp;
}
int64_t EventDetector::start_ts() const { return start_frame_ts_; }
bool EventDetectorConfig::defer_start(int num_obj) const {
  return minor_stride_ms * window_size < latest_duration_ms || num_obj < minimum_num;
}
}  // namespace avs_analyzer