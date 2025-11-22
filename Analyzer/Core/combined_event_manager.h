#ifndef ANALYZER_COMBINED_EVENT_MANAGER_H
#define ANALYZER_COMBINED_EVENT_MANAGER_H

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <mutex>

#include "event.h"
#include "frame.h"
#include "task.h"
#include "video_processing/dynamic_annotator.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {

struct CombinedEventConfig {
  int64_t pre_event_buffer_ms{0};
  int64_t post_event_buffer_ms{0};
  int64_t max_event_duration_ms{0};
  int64_t guard_window_ms{0};
};

class CombinedEventManager {
 public:
  struct DetectorFrameInfo {
    std::string_view monitor_code{};
    Task task;
    EventDetector::State state{EventDetector::kNoEvent};
    const std::vector<cv::Rect>* detections{nullptr};
    const std::vector<DynamicAnnotator::ROIEntry>* rois{nullptr};
    int64_t event_start_timestamp_ms{0};
    int64_t event_start_pts{0};
  };

  struct Result {
    std::unique_ptr<Frame> annotated_frame{};
    std::optional<std::unique_ptr<Event>> flushed_event{};
  };

  CombinedEventManager() = default;

  void UpdateConfig(const CombinedEventConfig& config);
  void SetAnnotator(const DynamicAnnotator* annotator) noexcept {
    annotator_ = annotator;
  }
  void SetAnnotatorMutex(std::mutex* m) noexcept { annotator_mutex_ = m; }

  Result ProcessFrame(const AVFrame* source_frame, int64_t timestamp_ms,
                      int64_t best_effort_pts,
                      const std::vector<DetectorFrameInfo>& detectors);

  std::vector<std::unique_ptr<Event>> FlushPending();

  [[nodiscard]] bool HasActiveEvent() const noexcept {
    return state_ != CombinedState::kIdle;
  }
  [[nodiscard]] bool HasAnnotator() const noexcept {
    return annotator_ != nullptr;
  }

 private:
  struct MonitorState {
    bool active{false};
    int64_t start_ts_ms{0};
    int64_t start_pts{0};
  };

  enum class CombinedState { kIdle, kActive, kEnding };

  std::unique_ptr<Event> FlushEvent(bool more_segments);
  void TrimFrameBuffer(int64_t current_timestamp_ms);
  [[nodiscard]] AVFramePtr MakeAnnotatedFrame(
    const AVFrame* source_frame, const std::vector<DetectorFrameInfo>& detectors,
    uint32_t& object_count) const;
  void UpdateCombinedStartFromActive(int64_t fallback_timestamp_ms,
                                     int64_t fallback_pts);

  CombinedEventConfig config_{};
  int64_t max_buffer_time_ms_{0};

  CombinedState state_{CombinedState::kIdle};
  std::deque<std::unique_ptr<Frame>> frame_buffer_{};
  std::unordered_map<std::string, MonitorState> monitor_state_{};
  bool has_active_monitors_{false};

  int64_t combined_event_start_ts_{0};
  int64_t combined_event_start_pts_{0};
  int64_t segment_start_ts_{0};
  int64_t segment_start_pts_{0};
  int64_t event_end_candidate_ts_{0};

  const DynamicAnnotator* annotator_{nullptr};
  std::mutex* annotator_mutex_{nullptr};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_COMBINED_EVENT_MANAGER_H
