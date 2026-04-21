#include "combined_event_manager.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <mutex>
#include <numeric>
#include <ranges>
#include <utility>

#include "Utils/log.h"
#include "dynamic_annotator.h"

namespace avs_analyzer {

namespace {
constexpr int64_t kBufferSafetyMarginMs = 1000;
}

void CombinedEventManager::UpdateConfig(const CombinedEventConfig& config) {
  config_ = config;
  if (config_.slice_ms > 0 && config_.max_event_duration_ms > 0 &&
      config_.max_event_duration_ms < config_.slice_ms) {
    LOGI("slice_ms ({}ms) adjusted: was > max_event_duration_ms ({}ms). "
         "Setting slice_ms to max_event_duration_ms for consistency.",
         config_.slice_ms, config_.max_event_duration_ms);
    config_.slice_ms = config_.max_event_duration_ms;
  }
  const int64_t guard_window =
      std::max(config_.slice_ms, config_.guard_window_ms);
  max_buffer_time_ms_ = config_.pre_event_buffer_ms + guard_window +
                        config_.post_event_buffer_ms + kBufferSafetyMarginMs;
}

CombinedEventManager::Result CombinedEventManager::ProcessFrame(
    const AVFrame* source_frame, int64_t timestamp_ms, int64_t best_effort_pts,
    const std::vector<DetectorFrameInfo>& detectors) {
  Result result{};
  if (!source_frame) {
    LOGE("Source frame is nullptr");
    return result;
  }

  TrimFrameBuffer(timestamp_ms);
  // Record previous combined state so we can report edge transitions to callers.
  const CombinedState prev_state = state_;

  // Update per-monitor states and track if any are active.
  bool has_active_monitors = false;
  for (const auto& info : detectors) {
    auto [it, _] =
        monitor_state_.try_emplace(std::string(info.monitor_code));
    auto& [active, start_ts_ms, start_pts] = it->second;

    switch (info.state) {
      case EventDetector::kEventStart:
      case EventDetector::kEventActive:
        active = true;
        if (info.event_start_timestamp_ms > 0) {
          start_ts_ms = info.event_start_timestamp_ms;
        }
        if (info.event_start_pts > 0) {
          start_pts = info.event_start_pts;
        }
        break;
      case EventDetector::kEventEnd:
        active = false;
        break;
      default:
        break;
    }

    if (active) {
      has_active_monitors = true;
    }
  }

  // Build annotated frame and store it in buffer.
  uint32_t object_count = 0;
  AVFramePtr annotated = MakeAnnotatedFrame(source_frame, detectors, object_count);
  if (!annotated) {
    annotated.reset(av_frame_clone(source_frame));
    if (!annotated) {
      LOGE("Failed to clone frame for event buffering fallback");
    }
  }
  if (annotated) {
    auto frame = std::make_shared<Frame>();
    frame->timestamp = timestamp_ms;
    frame->object_count = object_count;
    frame->data = std::move(annotated);
    if (frame->data) {
      frame->data->best_effort_timestamp = best_effort_pts;
      frame->data->pts = source_frame->pts;
      frame->data->time_base = source_frame->time_base;
    }

    std::shared_ptr<const Frame> const_frame = frame;
    if (frame->data) {
      result.annotated_frame = const_frame;
    }

    frame_buffer_.push_back(std::move(const_frame));
  }

  // Check if we've exceeded the max event duration - if so, flag it
  if (max_duration_state_ == MaxDuration::kNotExceeded && config_.max_event_duration_ms > 0 &&
      state_ == CombinedState::kActive &&
      timestamp_ms - combined_event_start_ts_ >= config_.max_event_duration_ms) {
    max_duration_state_ = MaxDuration::kHit;
    LOGI("Max event duration reached: {}ms for event starting at {}",
         timestamp_ms - combined_event_start_ts_, combined_event_start_ts_);
  }

  // Evaluate combined event state transitions.
  switch (state_) {
    case CombinedState::kIdle: {
      if (has_active_monitors) {
        state_ = CombinedState::kActive;
        ++event_id_counter_;
        UpdateCombinedStartFromActive(timestamp_ms, best_effort_pts);
        event_end_candidate_ts_ = 0;
      }
      break;
    }
    case CombinedState::kActive: {
      if (!has_active_monitors) {
        state_ = CombinedState::kEnding;
        event_end_candidate_ts_ = timestamp_ms;
      } else if (config_.slice_ms > 0 &&
                 timestamp_ms - segment_start_ts_ >= config_.slice_ms) {
        if (max_duration_state_ == MaxDuration::kHit) {
          result.transition = Result::Transition::kMaxDurationReached;
          if (auto event = FlushEvent(false)) {
            result.flushed_event = std::move(event);
          }
          max_duration_state_ = MaxDuration::kExceeded;
        } else if (max_duration_state_ == MaxDuration::kExceeded) {
          result.transition = Result::Transition::kMaxDurationReached;
        } else if (auto event = FlushEvent(true)) {
          result.flushed_event = std::move(event);
        }
        segment_start_ts_ = timestamp_ms;
        segment_start_pts_ = best_effort_pts;
      }
      break;
    }
    case CombinedState::kEnding: {
      if (has_active_monitors) {
        state_ = CombinedState::kActive;
        UpdateCombinedStartFromActive(timestamp_ms, best_effort_pts);
        event_end_candidate_ts_ = 0;
      } else if (timestamp_ms - event_end_candidate_ts_ >=
                 config_.post_event_buffer_ms) {
        if (max_duration_state_!= MaxDuration::kExceeded) {
          if (auto event = FlushEvent(false)) {
            result.flushed_event = std::move(event);
          }
        }
        state_ = CombinedState::kIdle;
        combined_event_start_ts_ = 0;
        combined_event_start_pts_ = 0;
        segment_start_ts_ = 0;
        segment_start_pts_ = 0;
        event_end_candidate_ts_ = 0;
        max_duration_state_ = MaxDuration::kNotExceeded;
      }
      break;
    }
  }

  // Report edge transitions for callers to take external actions. Only one
  // transition is represented per frame (idle->active, active->idle, or max duration).
  // Don't overwrite if a transition has already been set (e.g., kMaxDurationReached).
  if (result.transition == Result::Transition::kNone) {
    if (prev_state == CombinedState::kIdle && state_ == CombinedState::kActive) {
      result.transition = Result::Transition::kBecameActive;
    } else if (prev_state != CombinedState::kIdle && state_ == CombinedState::kIdle) {
      result.transition = Result::Transition::kBecameIdle;
    }
  }

  return result;
}

void CombinedEventManager::TrimFrameBuffer(int64_t current_timestamp_ms) {
  if (max_buffer_time_ms_ <= 0) {
    return;
  }
  while (!frame_buffer_.empty() &&
         current_timestamp_ms - frame_buffer_.front()->timestamp >
             max_buffer_time_ms_) {
    frame_buffer_.pop_front();
  }
}

AVFramePtr CombinedEventManager::MakeAnnotatedFrame(
    const AVFrame* source_frame, const std::vector<DetectorFrameInfo>& detectors,
    uint32_t& object_count) const {
  object_count = 0;
  if (!source_frame) {
    return nullptr;
  }

  AVFramePtr working{av_frame_clone(source_frame)};
  if (!working) {
    LOGE("Failed to clone frame for aggregated annotation");
    return nullptr;
  }
  if (!annotator_ || detectors.empty()) {
    return working;
  }
  for (const auto& info : detectors) {
    object_count += static_cast<uint32_t>(info.detections.size());
  }
  auto roi_count = std::accumulate(
    detectors.begin(), detectors.end(), size_t{0},
    [](size_t sum, const DetectorFrameInfo& info) {
      return sum + info.rois.size();
  });
  std::vector<DynamicAnnotator::BBoxEntry> bbox_entries;
  bbox_entries.reserve(object_count);
  std::vector<DynamicAnnotator::ROIEntry> roi_entries;
  roi_entries.reserve(roi_count);

  for (const auto& info : detectors) {
    for (const auto& rect : info.detections) {
      bbox_entries.emplace_back(info.task, rect);
    }
    roi_entries.insert(roi_entries.end(), info.rois.begin(), info.rois.end());
  }
  std::unique_lock<std::mutex> guard;
  if (annotator_mutex_) {
    guard = std::unique_lock<std::mutex>(*annotator_mutex_);
  }
  if (AVFramePtr annotated = annotator_->draw_elements(
          working.get(), roi_entries, bbox_entries)) {
    working = std::move(annotated);
  } else {
    LOGW("Failed to draw dynamic annotation for monitor {}", detectors.front().monitor_code);
  }
  return working;
}

void CombinedEventManager::UpdateCombinedStartFromActive(
    int64_t fallback_timestamp_ms, int64_t fallback_pts) {
  // Since we process frames sequentially, when transitioning from idle/ending
  // to active, the earliest active monitor's start is the combined start.
  int64_t min_ts = std::numeric_limits<int64_t>::max();
  int64_t min_pts = std::numeric_limits<int64_t>::max();
  
  for (const auto& [active, start_ts_ms, start_pts] : monitor_state_ | std::views::values) {
    if (active) {
      if (start_ts_ms > 0) {
        min_ts = std::min(min_ts, start_ts_ms);
      }
      if (start_pts > 0) {
        min_pts = std::min(min_pts, start_pts);
      }
    }
  }

  if (min_ts == std::numeric_limits<int64_t>::max()) {
    min_ts = fallback_timestamp_ms;
  }
  if (min_pts == std::numeric_limits<int64_t>::max()) {
    min_pts = fallback_pts;
  }

  combined_event_start_ts_ = min_ts;
  combined_event_start_pts_ = min_pts;
  segment_start_ts_ = min_ts;
  segment_start_pts_ = min_pts;
}

std::unique_ptr<Event> CombinedEventManager::FlushEvent(bool more_segments) {
  if (frame_buffer_.empty()) {
    return nullptr;
  }

  const int64_t threshold = segment_start_ts_ - config_.pre_event_buffer_ms;
  while (!frame_buffer_.empty() &&
         frame_buffer_.front()->timestamp < threshold) {
    frame_buffer_.pop_front();
  }

  if (frame_buffer_.empty()) {
    return nullptr;
  }

  auto event = std::make_unique<Event>();
  event->start_timestamp = segment_start_pts_;
  event->end_timestamp =
      frame_buffer_.back()->data ? frame_buffer_.back()->data->best_effort_timestamp
                                 : segment_start_pts_;
  event->more_segments = more_segments;

  while (!frame_buffer_.empty()) {
    event->frames.emplace_back(std::move(frame_buffer_.front()));
    frame_buffer_.pop_front();
  }

  return event;
}

std::vector<std::unique_ptr<Event>> CombinedEventManager::FlushPending() {
  std::vector<std::unique_ptr<Event>> events;
  if (frame_buffer_.empty() || segment_start_ts_ == 0) {
    frame_buffer_.clear();
  } else if (auto event = FlushEvent(false)) {
    events.emplace_back(std::move(event));
  }

  state_ = CombinedState::kIdle;
  combined_event_start_ts_ = 0;
  combined_event_start_pts_ = 0;
  segment_start_ts_ = 0;
  segment_start_pts_ = 0;
  event_end_candidate_ts_ = 0;
  max_duration_exceeded_ = false;

  return events;
}

}  // namespace avs_analyzer
