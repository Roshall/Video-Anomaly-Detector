#include "combined_event_manager.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

#include "Utils/log.h"

namespace avs_analyzer {

namespace {
constexpr int64_t kBufferSafetyMarginMs = 1000;
}

void CombinedEventManager::UpdateConfig(const CombinedEventConfig& config) {
  config_ = config;
  const int64_t guard_window =
      std::max(config_.max_event_duration_ms, config_.guard_window_ms);
  max_buffer_time_ms_ = config_.pre_event_buffer_ms + guard_window +
                        config_.post_event_buffer_ms + kBufferSafetyMarginMs;
}

CombinedEventManager::Result CombinedEventManager::ProcessFrame(
    AVFrame* source_frame, int64_t timestamp_ms, int64_t best_effort_pts,
    const std::vector<DetectorFrameInfo>& detectors) {
  Result result{};

  TrimFrameBuffer(timestamp_ms);

  // Update per-monitor states and track if any are active.
  has_active_monitors_ = false;
  for (const auto& info : detectors) {
    auto [it, _] =
        monitor_state_.try_emplace(std::string(info.monitor_code));
    auto& status = it->second;

    switch (info.state) {
      case EventDetector::kEventStart:
      case EventDetector::kEventActive:
        status.active = true;
        if (info.event_start_timestamp_ms > 0) {
          status.start_ts_ms = info.event_start_timestamp_ms;
        }
        if (info.event_start_pts > 0) {
          status.start_pts = info.event_start_pts;
        }
        break;
      case EventDetector::kEventEnd:
        status.active = false;
        break;
      default:
        break;
    }

    if (status.active) {
      has_active_monitors_ = true;
    }
  }

  // Build annotated frame and store it in buffer.
  uint32_t object_count = 0;
  AVFramePtr annotated = MakeAnnotatedFrame(source_frame, detectors, object_count);
  if (!annotated && source_frame) {
    annotated.reset(av_frame_clone(source_frame));
    if (!annotated) {
      LOGE("Failed to clone frame for event buffering fallback");
    }
  }
  if (annotated) {
    auto frame = std::make_unique<Frame>();
    frame->timestamp = timestamp_ms;
    frame->object_count = object_count;
    frame->data = std::move(annotated);
    if (frame->data) {
      frame->data->best_effort_timestamp = best_effort_pts;
      if (source_frame) {
        frame->data->pts = source_frame->pts;
        frame->data->time_base = source_frame->time_base;
      }
    }

    // Prepare frame for push stream consumers.
    if (frame->data) {
      if (AVFramePtr push_clone{av_frame_clone(frame->data.get())}) {
        push_clone->best_effort_timestamp = best_effort_pts;
        if (source_frame) {
          push_clone->pts = source_frame->pts;
          push_clone->time_base = source_frame->time_base;
        }
        auto push_frame = std::make_unique<Frame>();
        push_frame->timestamp = timestamp_ms;
        push_frame->object_count = object_count;
        push_frame->data = std::move(push_clone);
        result.annotated_frame = std::move(push_frame);
      }
    }

    frame_buffer_.push_back(std::move(frame));
  }

  // Evaluate combined event state transitions.
  switch (state_) {
    case CombinedState::kIdle: {
      if (has_active_monitors_) {
        state_ = CombinedState::kActive;
        UpdateCombinedStartFromActive(timestamp_ms, best_effort_pts);
        event_end_candidate_ts_ = 0;
      }
      break;
    }
    case CombinedState::kActive: {
      if (!has_active_monitors_) {
        state_ = CombinedState::kEnding;
        event_end_candidate_ts_ = timestamp_ms;
      } else if (timestamp_ms - segment_start_ts_ >=
                     config_.max_event_duration_ms) {
        if (auto event = FlushEvent(true)) {
          result.flushed_events.emplace_back(std::move(event));
        }
        segment_start_ts_ = timestamp_ms;
        segment_start_pts_ = best_effort_pts;
      }
      break;
    }
    case CombinedState::kEnding: {
      if (has_active_monitors_) {
        state_ = CombinedState::kActive;
        UpdateCombinedStartFromActive(timestamp_ms, best_effort_pts);
        event_end_candidate_ts_ = 0;
      } else if (timestamp_ms - event_end_candidate_ts_ >=
                 config_.post_event_buffer_ms) {
        if (auto event = FlushEvent(false)) {
          result.flushed_events.emplace_back(std::move(event));
        }
        state_ = CombinedState::kIdle;
        combined_event_start_ts_ = 0;
        combined_event_start_pts_ = 0;
        segment_start_ts_ = 0;
        segment_start_pts_ = 0;
        event_end_candidate_ts_ = 0;
      }
      break;
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

  static constexpr std::vector<cv::Rect> kEmptyRects{};
  // TODO: use different color for different task, also add a legend.
  for (const auto& info : detectors) {
    if (!info.overlay) {
      continue;
    }
    const auto* rects = info.detections ? info.detections : &kEmptyRects;
    AVFramePtr annotated = info.overlay->draw_elements(working.get(), *rects, 4);
    if (annotated) {
      working = std::move(annotated);
    } else {
      LOGW("Failed to draw overlay for monitor {}", info.monitor_code);
    }
    object_count += static_cast<uint32_t>(rects->size());
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
  has_active_monitors_ = false;
  combined_event_start_ts_ = 0;
  combined_event_start_pts_ = 0;
  segment_start_ts_ = 0;
  segment_start_pts_ = 0;
  event_end_candidate_ts_ = 0;

  return events;
}

}  // namespace avs_analyzer
