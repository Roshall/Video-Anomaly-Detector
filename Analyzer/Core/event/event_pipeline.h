#ifndef ANALYZER_EVENT_PIPELINE_H
#define ANALYZER_EVENT_PIPELINE_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "combined_event_manager.h"
#include "frame.h"
#include "monitor/monitor_registry.h"
#include "task.h"
#include "video_processing/dynamic_annotator.h"
#include "video_processing/video_context.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {
class Scheduler;
struct WorkerMetrics;

// One image that must be uploaded for a monitor whose event just started.
struct AlarmRequest {
  std::string monitor_code;
  int64_t start_pts{0};
  int64_t event_pts{0};
  int level_index{0};
  std::string level;
  int64_t alarm_id{0};
  std::shared_ptr<const Frame> frame;
};

// Everything the detect loop needs from one decoded frame.
struct EventPipelineResult {
  std::shared_ptr<const Frame> annotated_frame;
  std::optional<std::unique_ptr<Event>> flushed_event;
  CombinedEventManager::Result::Transition transition{
      CombinedEventManager::Result::Transition::kNone};
  int64_t event_id{-1};
  std::vector<AlarmRequest> alarms;
};

// Owns the per-frame detection/aggregation path: monitor detectors -> combined
// event manager -> annotator. It does not perform alarm I/O or stream output.
class EventPipeline {
 public:
  EventPipeline(Scheduler* scheduler, std::string video_code, bool file,
                std::shared_ptr<WorkerMetrics> metrics);

  // Recomputes the combined event config from the current monitor set.
  void UpdateConfig(const std::vector<MonitorRegistry::Ptr>& monitors,
                    bool offline_mode = false);
  void UpdateMemoryBudget(size_t bytes);

  // Builds a new annotator for `video_context` and publishes it to the event
  // manager. Safe to call from control threads while the detect thread runs.
  void EnsureAnnotator(const VideoContext& video_context,
                       const std::vector<std::pair<Task, std::u8string>>& tasks,
                       bool force_rebuild = false);

  [[nodiscard]] EventPipelineResult ProcessFrame(
      const AVFrame* frame, int64_t timestamp_ms, int64_t best_effort_pts,
      const std::vector<MonitorRegistry::Ptr>& monitors);

  std::vector<std::unique_ptr<Event>> FlushPending();
  [[nodiscard]] int64_t CurrentEventId() const;
  [[nodiscard]] bool HasActiveEvent() const;
  [[nodiscard]] bool HasAnnotator() const;

 private:
  [[nodiscard]] size_t EventBufferBudgetBytes() const;
  static int ComputeLevelIndex(
      const std::pair<std::vector<int>, std::vector<std::string>>& levels,
      LevelMode level_mode, int64_t num_detection, int64_t event_duration,
      std::string_view monitor_code);

  Scheduler* scheduler_{nullptr};
  std::string video_code_;
  bool file_{false};
  std::shared_ptr<WorkerMetrics> metrics_;

  std::shared_ptr<CombinedEventManager> event_manager_;
  std::shared_ptr<DynamicAnnotator> annotator_;
  mutable std::mutex annotator_mutex_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_EVENT_PIPELINE_H
