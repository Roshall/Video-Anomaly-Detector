#include "combined_event_manager.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <mutex>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <utility>

#include "Utils/log.h"
#include "dynamic_annotator.h"
#include "jpeg_encoder.h"

namespace avs_analyzer {

namespace {
constexpr int64_t kBufferSafetyMarginMs = 1000;
// Buffered pre-roll frames are never displayed directly, so a moderately
// aggressive quantiser keeps them small without hurting event detection.
constexpr int kBufferJpegQuality = 8;

size_t ApproxFrameBytes(const AVFrame* frame) {
  if (!frame || frame->width <= 0 || frame->height <= 0) return 0;
  const int size = av_image_get_buffer_size(
      static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, 1);
  if (size > 0) return static_cast<size_t>(size);
  // Hardware / unusual formats: fall back to the size of 4:2:0 video.
  return static_cast<size_t>(frame->width) * frame->height * 3 / 2;
}

size_t FrameStorageBytes(const Frame& frame) {
  return sizeof(Frame) + (frame.encoded ? frame.jpeg.size()
                                        : ApproxFrameBytes(frame.data.get()));
}
}  // namespace

CombinedEventManager::CombinedEventManager() = default;

CombinedEventManager::~CombinedEventManager() = default;

void CombinedEventManager::UpdateMemoryBudget(size_t bytes) {
  memory_budget_bytes_.store(bytes, std::memory_order_release);
}

bool CombinedEventManager::has_raw_buffered_frames() const noexcept {
  return std::ranges::any_of(frame_buffer_, [](const auto& frame) {
    return frame && !frame->encoded;
  });
}

void CombinedEventManager::UpdateConfig(const CombinedEventConfig& config) {
  CombinedEventConfig adjusted = config;
  if (adjusted.slice_ms > 0 && adjusted.max_event_duration_ms > 0 &&
      adjusted.max_event_duration_ms < adjusted.slice_ms) {
    LOGI("slice_ms ({}ms) adjusted: was > max_event_duration_ms ({}ms). "
         "Setting slice_ms to max_event_duration_ms for consistency.",
         adjusted.slice_ms, adjusted.max_event_duration_ms);
    adjusted.slice_ms = adjusted.max_event_duration_ms;
  }
  const int64_t guard_window =
      std::max(adjusted.slice_ms, adjusted.guard_window_ms);
  max_buffer_time_ms_.store(adjusted.pre_event_buffer_ms + guard_window +
                                adjusted.post_event_buffer_ms +
                                kBufferSafetyMarginMs,
                            std::memory_order_release);
  config_.store(adjusted, std::memory_order_release);
}

void CombinedEventManager::SetAnnotator(
    std::shared_ptr<DynamicAnnotator> annotator) noexcept {
  std::lock_guard lock(annotator_mutex_);
  annotator_ = std::move(annotator);
}

CombinedEventManager::Result CombinedEventManager::ProcessFrame(
    const AVFrame* source_frame, int64_t timestamp_ms, int64_t best_effort_pts,
    const std::vector<DetectorFrameInfo>& detectors) {
  Result result{};
  if (!source_frame) {
    LOGE("Source frame is nullptr");
    return result;
  }

  // Snapshot the config once so one frame sees a consistent set of values.
  const auto config = config_.load(std::memory_order_acquire);
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

  // Build the frame used both for live output and, when required, for the
  // event buffer. Idle frames without detections are stored JPEG-encoded so
  // an idle stream does not retain seconds of raw 1080p frames.
  uint32_t object_count = 0;
  AVFramePtr rendered;
  if (!detectors.empty()) {
    rendered = MakeAnnotatedFrame(source_frame, detectors, object_count);
  }
  if (!rendered) {
    rendered.reset(av_frame_clone(source_frame));
    if (!rendered) {
      LOGE("Failed to clone frame for event buffering fallback");
    }
  }
  if (rendered) {
    rendered->best_effort_timestamp = best_effort_pts;
    rendered->pts = source_frame->pts;
    rendered->time_base = source_frame->time_base;

    auto output = std::make_shared<Frame>();
    output->timestamp = timestamp_ms;
    output->object_count = object_count;
    output->pts = source_frame->pts;
    output->best_effort_pts = best_effort_pts;
    output->time_base = source_frame->time_base;
    output->data = std::move(rendered);
    output->byte_size = FrameStorageBytes(*output);

    result.annotated_frame = output;

    const bool idle_without_detections = !has_active_monitors &&
                                         detectors.empty() &&
                                         state_ == CombinedState::kIdle;
    if (idle_without_detections) {
      auto stored = std::make_shared<Frame>();
      stored->timestamp = timestamp_ms;
      stored->object_count = object_count;
      stored->pts = output->pts;
      stored->best_effort_pts = best_effort_pts;
      stored->time_base = source_frame->time_base;
      stored->jpeg = EncodeIdleFrame(output->data.get());
      stored->encoded = true;
      if (stored->jpeg.empty()) {
        // Encoding failed: keep a raw clone so event footage is not lost.
        stored->encoded = false;
        stored->data.reset(av_frame_clone(output->data.get()));
        if (stored->data) {
          stored->data->best_effort_timestamp = best_effort_pts;
          stored->data->pts = source_frame->pts;
          stored->data->time_base = source_frame->time_base;
        }
      }
      if (stored->encoded || stored->data) {
        stored->byte_size = FrameStorageBytes(*stored);
        AppendFrame(std::move(stored));
      }
    } else {
      AppendFrame(std::move(output));
    }
  }

  // Enforce the time window and byte budget after this frame is buffered.
  TrimFrameBuffer(timestamp_ms);

  // Check if we've exceeded the max event duration - if so, flag it
  if (max_duration_state_ == MaxDuration::kNotExceeded && config.max_event_duration_ms > 0 &&
      state_ == CombinedState::kActive &&
      timestamp_ms - combined_event_start_ts_ >= config.max_event_duration_ms) {
    max_duration_state_ = MaxDuration::kHit;
    LOGI("Max event duration reached: {}ms for event starting at {}",
         timestamp_ms - combined_event_start_ts_, combined_event_start_ts_);
  }

  // Evaluate combined event state transitions.
  switch (state_) {
    case CombinedState::kIdle: {
      if (has_active_monitors) {
        state_ = CombinedState::kActive;
        event_id_counter_ = NextAlarmId();
        UpdateCombinedStartFromActive(timestamp_ms, best_effort_pts);
        event_end_candidate_ts_ = 0;
      }
      break;
    }
    case CombinedState::kActive: {
      if (!has_active_monitors) {
        state_ = CombinedState::kEnding;
        event_end_candidate_ts_ = timestamp_ms;
      } else if (config.slice_ms > 0 &&
                 timestamp_ms - segment_start_ts_ >= config.slice_ms) {
        if (max_duration_state_ == MaxDuration::kHit) {
          result.transition = Result::Transition::kMaxDurationReached;
          if (auto event = FlushEvent(false)) {
            result.flushed_event = std::move(event);
          }
          max_duration_state_ = MaxDuration::kExceeded;
        } else if (max_duration_state_ == MaxDuration::kExceeded) {
          // Do nothing, already exceeded and reported
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
                 config.post_event_buffer_ms) {
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

int64_t CombinedEventManager::CurrentBufferWindowMs(
    const CombinedEventConfig& config, int64_t current_timestamp_ms) const {
  switch (state_) {
    case CombinedState::kIdle:
      return config.pre_event_buffer_ms + config.guard_window_ms +
             kBufferSafetyMarginMs;
    case CombinedState::kActive: {
      // Flushes happen every slice, so keeping (now - segment_start) + pre
      // covers both the guard delay at detection time and the first flush.
      const int64_t elapsed =
          std::max<int64_t>(0, current_timestamp_ms - segment_start_ts_);
      return elapsed + config.pre_event_buffer_ms + kBufferSafetyMarginMs;
    }
    case CombinedState::kEnding: {
      // Also retain the post-event tail until the final flush.
      const int64_t elapsed =
          std::max<int64_t>(0, current_timestamp_ms - segment_start_ts_);
      return elapsed + config.pre_event_buffer_ms +
             config.post_event_buffer_ms + kBufferSafetyMarginMs;
    }
  }
  return config.pre_event_buffer_ms + kBufferSafetyMarginMs;
}

void CombinedEventManager::AppendFrame(std::shared_ptr<const Frame> frame) {
  if (!frame) {
    return;
  }
  buffer_bytes_ += frame->byte_size;
  frame_buffer_.push_back(std::move(frame));
}

void CombinedEventManager::DropOldestFrame() {
  if (frame_buffer_.empty()) {
    return;
  }
  buffer_bytes_ -= std::min(buffer_bytes_, frame_buffer_.front()->byte_size);
  frame_buffer_.pop_front();
}

void CombinedEventManager::TrimFrameBuffer(int64_t current_timestamp_ms) {
  const auto config = config_.load(std::memory_order_acquire);
  const size_t budget_bytes =
      memory_budget_bytes_.load(std::memory_order_acquire);
  int64_t window = CurrentBufferWindowMs(config, current_timestamp_ms);
  const int64_t hard_cap = max_buffer_time_ms_.load(std::memory_order_acquire);
  if (hard_cap > 0) {
    window = window > 0 ? std::min(window, hard_cap) : hard_cap;
  }

  // The byte budget bounds the idle pre-roll (JPEG-encoded, small).
  // While an event is active or ending, frames are raw and the required
  // window is bounded by the slice cadence instead: evicting there would
  // truncate the recorded clip, dropping the trigger moment itself. The
  // hard time cap below still bounds memory in every state.
  const bool budget_applies = state_ == CombinedState::kIdle;

  while (!frame_buffer_.empty()) {
    const bool over_time =
        window > 0 &&
        current_timestamp_ms - frame_buffer_.front()->timestamp > window;
    const bool over_budget =
        budget_applies && budget_bytes > 0 && buffer_bytes_ > budget_bytes;
    if (!over_time && !over_budget) {
      break;
    }
    if (over_budget) {
      ++buffer_evictions_;
      if (buffer_evictions_ == 1 || buffer_evictions_ % 200 == 0) {
        LOGW("Event buffer budget exceeded ({} MB): evicted {} frames",
             budget_bytes / (1024 * 1024), buffer_evictions_);
      }
    }
    DropOldestFrame();
  }
}

std::vector<uint8_t> CombinedEventManager::EncodeIdleFrame(
    const AVFrame* frame) {
  if (!frame) {
    return {};
  }
  try {
    if (!buffer_codec_ || buffer_codec_width_ != frame->width ||
        buffer_codec_height_ != frame->height) {
      buffer_codec_ = std::make_unique<JPEGEncoder>(
          frame->width, frame->height, kBufferJpegQuality);
      buffer_codec_width_ = frame->width;
      buffer_codec_height_ = frame->height;
    }
    return buffer_codec_->EncodeToBuffer(const_cast<AVFrame*>(frame));
  } catch (const std::exception& e) {
    LOGE("Failed to initialise buffered JPEG encoder: {}", e.what());
    buffer_codec_.reset();
    buffer_codec_width_ = 0;
    buffer_codec_height_ = 0;
    return {};
  }
}

std::shared_ptr<const Frame> CombinedEventManager::MaterializeFrame(
    const std::shared_ptr<const Frame>& stored) {
  if (!stored || !stored->encoded) {
    return stored;
  }
  if (!buffer_codec_) {
    return nullptr;
  }
  AVFramePtr decoded = buffer_codec_->DecodeToFrame(stored->jpeg);
  if (!decoded) {
    LOGW("Failed to decode buffered frame at {} ms", stored->timestamp);
    return nullptr;
  }
  decoded->best_effort_timestamp = stored->best_effort_pts;
  decoded->pts = stored->pts;
  decoded->time_base = stored->time_base;

  auto frame = std::make_shared<Frame>();
  frame->timestamp = stored->timestamp;
  frame->object_count = stored->object_count;
  frame->pts = stored->pts;
  frame->best_effort_pts = stored->best_effort_pts;
  frame->time_base = stored->time_base;
  frame->data = std::move(decoded);
  frame->byte_size = FrameStorageBytes(*frame);
  return frame;
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
  // Published annotators are immutable; copy the pointer under the short lock
  // and draw without holding it, so a rebuild never blocks the detect thread.
  std::shared_ptr<DynamicAnnotator> annotator;
  {
    std::lock_guard lock(annotator_mutex_);
    annotator = annotator_;
  }
  if (!annotator || detectors.empty()) {
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
  if (AVFramePtr annotated = annotator->draw_elements(
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

  const int64_t threshold =
      segment_start_ts_ - config_.load(std::memory_order_relaxed).pre_event_buffer_ms;
  while (!frame_buffer_.empty() &&
         frame_buffer_.front()->timestamp < threshold) {
    DropOldestFrame();
  }

  if (frame_buffer_.empty()) {
    return nullptr;
  }

  // Materialise encoded entries before handing the frames to the recorders.
  std::vector<std::shared_ptr<const Frame>> frames;
  frames.reserve(frame_buffer_.size());
  for (const auto& stored : frame_buffer_) {
    if (auto frame = MaterializeFrame(stored)) {
      frames.emplace_back(std::move(frame));
    }
  }
  frame_buffer_.clear();
  buffer_bytes_ = 0;
  if (frames.empty()) {
    return nullptr;
  }

  auto event = std::make_unique<Event>();
  event->start_timestamp = segment_start_pts_;
  event->end_timestamp = frames.back()->best_effort_pts;
  event->more_segments = more_segments;
  event->frames = std::move(frames);

  return event;
}

std::vector<std::unique_ptr<Event>> CombinedEventManager::FlushPending() {
  std::vector<std::unique_ptr<Event>> events;
  if (frame_buffer_.empty() || segment_start_ts_ == 0) {
    frame_buffer_.clear();
    buffer_bytes_ = 0;
  } else if (auto event = FlushEvent(false)) {
    events.emplace_back(std::move(event));
  }

  state_ = CombinedState::kIdle;
  combined_event_start_ts_ = 0;
  combined_event_start_pts_ = 0;
  segment_start_ts_ = 0;
  segment_start_pts_ = 0;
  event_end_candidate_ts_ = 0;
  max_duration_state_ = MaxDuration::kNotExceeded;

  return events;
}

}  // namespace avs_analyzer
