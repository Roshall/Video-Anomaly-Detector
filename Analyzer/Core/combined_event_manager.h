#ifndef ANALYZER_COMBINED_EVENT_MANAGER_H
#define ANALYZER_COMBINED_EVENT_MANAGER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <mutex>

// Event definition moved into the avs_analyzer namespace below.
#include "Utils/common.h"
#include "frame.h"
#include "task.h"
#include "event.h"
#include "video_processing/dynamic_annotator.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {

class JPEGEncoder;

// Represents a detected event, containing all relevant frames.
struct Event {
  int64_t start_timestamp{};
  int64_t end_timestamp{};
  bool more_segments{false};

  std::vector<std::shared_ptr<const Frame>> frames{};

  [[nodiscard]] int64_t GetDuration() const {
    return end_timestamp - start_timestamp;
  }
};

struct CombinedEventConfig {
  int64_t pre_event_buffer_ms{0};
  int64_t post_event_buffer_ms{0};
  int64_t max_event_duration_ms{0};
  int64_t slice_ms{3000};
  int64_t guard_window_ms{0};
};

// Read on the detect thread for every frame; updated by the control thread.
static_assert(std::is_trivially_copyable_v<CombinedEventConfig>);

class CombinedEventManager {
 public:
  struct DetectorFrameInfo {
    std::string_view monitor_code{};
    Task task;
    EventDetector::State state{EventDetector::kNoEvent};
    std::vector<cv::Rect> detections{};
    std::vector<DynamicAnnotator::ROIEntry> rois{};
    int64_t event_start_timestamp_ms{0};
    int64_t event_start_pts{0};
  };

  struct Result {
    std::shared_ptr<const Frame> annotated_frame{};
    std::optional<std::unique_ptr<Event>> flushed_event{};
    // Edge transition reported as an enum to indicate a single transition per
    // frame: none, became active, or became idle.
    enum class Transition { kNone, kBecameActive, kBecameIdle, kMaxDurationReached };
    Transition transition{Transition::kNone};
  };

  CombinedEventManager();
  ~CombinedEventManager();

  void UpdateConfig(const CombinedEventConfig& config);
  // Caps the bytes retained by the frame buffer. 0 disables the byte budget.
  void UpdateMemoryBudget(size_t bytes);
  // MJPEG quality_q (2..31, lower = better) for buffered idle frames.
  void UpdateBufferJpegQuality(int quality_q);
  void SetAnnotator(std::shared_ptr<DynamicAnnotator> annotator) noexcept;

  Result ProcessFrame(const AVFrame* source_frame, int64_t timestamp_ms,
                      int64_t best_effort_pts,
                      const std::vector<DetectorFrameInfo>& detectors);

  std::vector<std::unique_ptr<Event>> FlushPending();

  [[nodiscard]] bool HasActiveEvent() const noexcept {
    return state_ != CombinedState::kIdle;
  }
  [[nodiscard]] bool HasAnnotator() const noexcept {
    std::lock_guard lock(annotator_mutex_);
    return annotator_ != nullptr;
  }
  [[nodiscard]] int64_t CurrentEventId() const noexcept {
    return event_id_counter_;
  }

  // Diagnostics / tests.
  [[nodiscard]] size_t buffer_bytes() const noexcept { return buffer_bytes_; }
  [[nodiscard]] size_t buffer_frames() const noexcept {
    return frame_buffer_.size();
  }
  [[nodiscard]] uint64_t buffer_evictions() const noexcept {
    return buffer_evictions_;
  }
  [[nodiscard]] bool has_raw_buffered_frames() const noexcept;

 private:
  struct MonitorState {
    bool active{false};
    int64_t start_ts_ms{0};
    int64_t start_pts{0};
  };

  enum class CombinedState { kIdle, kActive, kEnding };
  enum class MaxDuration { kNotExceeded, kHit, kExceeded };

  std::unique_ptr<Event> FlushEvent(bool more_segments);
  void TrimFrameBuffer(int64_t current_timestamp_ms);
  // Time window that must be retained up to `current_timestamp_ms`. Idle
  // keeps the configured pre-roll (plus detector guard); active/ending keep
  // everything back to `segment_start - pre` so the first/last flush has its
  // full pre-roll.
  [[nodiscard]] int64_t CurrentBufferWindowMs(
      const CombinedEventConfig& config, int64_t current_timestamp_ms) const;
  void AppendFrame(std::shared_ptr<const Frame> frame);
  void DropOldestFrame();
  // Returns a frame with `data` populated, decoding JPEG entries on demand.
  std::shared_ptr<const Frame> MaterializeFrame(
      const std::shared_ptr<const Frame>& stored);
  std::vector<uint8_t> EncodeIdleFrame(const AVFrame* frame);
  [[nodiscard]] AVFramePtr MakeAnnotatedFrame(
    const AVFrame* source_frame, const std::vector<DetectorFrameInfo>& detectors,
    uint32_t& object_count) const;
  void UpdateCombinedStartFromActive(int64_t fallback_timestamp_ms,
                                     int64_t fallback_pts);

  std::atomic<CombinedEventConfig> config_{CombinedEventConfig{}};
  std::atomic<int64_t> max_buffer_time_ms_{0};

  CombinedState state_{CombinedState::kIdle};
  std::deque<std::shared_ptr<const Frame>> frame_buffer_{};
  size_t buffer_bytes_{0};
  // Written by the control thread, read by the detect thread. 0 = unlimited.
  std::atomic<size_t> memory_budget_bytes_{0};
  std::atomic<int> buffer_jpeg_quality_{8};
  uint64_t buffer_evictions_{0};
  // Encodes idle frames and decodes them again at flush time. Owned by the
  // event manager, so only the detect thread touches it.
  std::unique_ptr<JPEGEncoder> buffer_codec_{};
  int buffer_codec_width_{0};
  int buffer_codec_quality_{-1};
  int buffer_codec_height_{0};
  std::unordered_map<std::string, MonitorState> monitor_state_{};
  MaxDuration max_duration_state_{MaxDuration::kNotExceeded};

  int64_t combined_event_start_ts_{0};
  int64_t combined_event_start_pts_{0};
  int64_t segment_start_ts_{0};
  int64_t segment_start_pts_{0};
  int64_t event_end_candidate_ts_{0};
  int64_t event_id_counter_{0};

  // Published annotator instances are immutable; the mutex only protects the
  // pointer while the detect thread copies it.
  mutable std::mutex annotator_mutex_;
  std::shared_ptr<DynamicAnnotator> annotator_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_COMBINED_EVENT_MANAGER_H
