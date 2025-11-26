#include "event.h"

#include "Utils/log.h"
#include "format_convertor.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <analyzer.h>

namespace avs_analyzer {

EventDetector::EventDetector(const EventDetectorConfig& config,
                             const Analyzer& analyzer,
                             FormatConvertor convertor)
    : config_(config),
      convertor_(std::move(convertor)),
      analyzer_(analyzer),
      object_votes_window_(config.window_size) {
  last_major_check_ts_ = -config_.major_stride_ms;
}

std::optional<std::pair<int64_t, int64_t>>
EventDetector::VotesWindow::first_positive_timestamp() const {
  for (const auto& sample : samples_) {
    if (sample.positive) {
      return std::pair<int64_t, int64_t>{sample.timestamp,
                                         sample.best_effort_timestamp};
    }
  }
  return std::nullopt;
}

std::pair<int64_t, int64_t> EventDetector::FindFirstVoteTimestamp() const {
  if (const auto ts = object_votes_window_.first_positive_timestamp()) {
    return *ts;
  }
  return {last_timestamp_ms_, last_best_effort_ts_};
}

EventDetector::State EventDetector::ProcessFrame(const int64_t current_ts, const int64_t best_effort_ts) {
  State state{kNoEvent};
  last_timestamp_ms_ = current_ts;
  last_best_effort_ts_ = best_effort_ts;

  switch (current_state_) {
    case InnerState::kMajorCheck: {
      if (current_ts - last_major_check_ts_ >= config_.major_stride_ms) {
        last_major_check_ts_ = current_ts;
        if (auto bboxes = Detect(frame.data); not bboxes.empty()) {
          LOGD("[Major] Object found at t = {}s. Switching to MINOR_CHECK.",
               current_ts);
          current_state_ = InnerState::kMinorCheck;
          last_minor_check_ts_ = current_ts;
          object_votes_window_.slide(true, current_ts, best_effort_ts);
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
        object_votes_window_.slide(has_obj, current_ts, best_effort_ts);
        if (object_votes_window_.full()) {
          if (auto votes = object_votes_window_.votes();
              votes >= config_.min_vote) {
            const auto [start_ts, start_pts] = FindFirstVoteTimestamp();
            active_event_start_ts_ = start_ts;
            active_event_start_pts_ = start_pts;
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
        object_votes_window_.slide(has_obj, current_ts, best_effort_ts);
        if (has_obj) {
          kept_bboxes_ = std::move(bboxes);
        }

    if (auto votes = object_votes_window_.votes();
      votes >= config_.min_vote) {
          if (current_ts - active_event_start_ts_ >= config_.latest_duration_ms &&
              kept_bboxes_.size() >= static_cast<size_t>(config_.minimum_num)) {
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
        object_votes_window_.slide(has_obj, current_ts, best_effort_ts);
        if (has_obj) {
          kept_bboxes_ = std::move(bboxes);
        }

        state = kEventActive;
        // Check for event end condition
        if (auto votes = object_votes_window_.votes();
            votes < config_.min_vote || kept_bboxes_.size() < static_cast<size_t>(config_.minimum_num)) {
          LOGD("[Active] Event ending at t = {}ms. Votes dropped to {}.",
               current_ts, votes);
          // Transition directly to appropriate check state
          current_state_ = kept_bboxes_.empty() ? InnerState::kMajorCheck
                                                : InnerState::kDoubleCheck;
          last_major_check_ts_ = current_ts;
          object_votes_window_.clear();
          kept_bboxes_.clear();
          state = kEventEnd;
        }
      }
      break;
    }
    default:
      break;
  }

  return state;
}

std::vector<cv::Rect> EventDetector::Detect(AVFrame* frame) {
  return analyzer_.ProcessVideoFrame(convertor_(frame));
}
int64_t EventDetector::timestamp() const {
  return last_best_effort_ts_;
}
int64_t EventDetector::start_ts() const { return active_event_start_pts_; }
bool EventDetectorConfig::defer_start(int num_obj) const {
  return minor_stride_ms < latest_duration_ms || num_obj < minimum_num;
}
}  // namespace avs_analyzer