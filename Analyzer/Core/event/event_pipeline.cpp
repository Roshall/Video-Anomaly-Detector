#include "event/event_pipeline.h"

#include "admission.h"
#include "video_processing/format_convertor.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <format>
#include <string_view>

#include "Utils/log.h"
#include "analyzer.h"
#include "config.h"
#include "event.h"
#include "scheduler.h"

namespace avs_analyzer {
namespace {

// Snapshot of one monitor's detector state for alarm handling. The context
// pointer keeps the monitor alive until the frame is fully processed.
struct MonitorEventSnapshot {
  MonitorRegistry::Ptr ctx;
  std::string monitor_code;
  EventDetector::State state;
  int64_t start_pts;
  int64_t event_pts;
  int64_t num_detection;
  int64_t event_duration;
  std::pair<std::vector<int>, std::vector<std::string>> levels;
  LevelMode level_mode;
  int current_highest_level;
};

}  // namespace

EventPipeline::EventPipeline(Scheduler* scheduler, std::string video_code,
                             uint64_t video_id, bool file,
                             std::shared_ptr<WorkerMetrics> metrics)
    : scheduler_(scheduler),
      video_code_(std::move(video_code)),
      video_id_(video_id),
      file_(file),
      metrics_(std::move(metrics)) {}

size_t EventPipeline::EventBufferBudgetBytes() const {
  if (!scheduler_) {
    return 256ull * 1024 * 1024;
  }
  return static_cast<size_t>(
             scheduler_->GetConfig().event_buffer_budget_mb) *
         1024 * 1024;
}

void EventPipeline::UpdateConfig(
    const std::vector<MonitorRegistry::Ptr>& monitors, bool offline_mode) {
  // Monitor set/config changed: allow a fixed-then-broken monitor to warn
  // again instead of staying silent for the pipeline's lifetime. The set
  // itself belongs to the detect thread, so hand the reset over via a flag.
  level_log_reset_.store(true, std::memory_order_relaxed);
  CombinedEventConfig cfg{};
  for (const auto& ctx : monitors) {
    if (!ctx) {
      continue;
    }
    const auto& event_cfg = ctx->event_cfg;
    cfg.pre_event_buffer_ms =
        std::max(cfg.pre_event_buffer_ms, event_cfg.pre_event_buffer_ms);
    cfg.post_event_buffer_ms =
        std::max(cfg.post_event_buffer_ms, event_cfg.post_event_buffer_ms);
    cfg.max_event_duration_ms =
        std::max(cfg.max_event_duration_ms, event_cfg.max_event_duration_ms);
    const int64_t guard = std::max(
        static_cast<int64_t>(event_cfg.minor_stride_ms) * event_cfg.window_size,
        event_cfg.latest_duration_ms);
    cfg.guard_window_ms = std::max(cfg.guard_window_ms, guard);
  }
  if (scheduler_) {
    cfg.slice_ms = scheduler_->GetConfig().hls_config.segment_seconds * 1000;
  }
  if (offline_mode && cfg.max_event_duration_ms > 0) {
    cfg.slice_ms = cfg.max_event_duration_ms;
  }

  auto manager = std::atomic_load(&event_manager_);
  if (!manager) {
    manager = std::make_shared<CombinedEventManager>();
    std::shared_ptr<DynamicAnnotator> annotator;
    {
      std::lock_guard lock(annotator_mutex_);
      annotator = annotator_;
    }
    manager->SetAnnotator(std::move(annotator));
    std::atomic_store(&event_manager_, manager);
  }
  manager->UpdateConfig(cfg);
  manager->UpdateMemoryBudget(EventBufferBudgetBytes());
  if (scheduler_) {
    manager->UpdateBufferJpegQuality(
        scheduler_->GetConfig().event_buffer_jpeg_quality);
  }
}

void EventPipeline::UpdateMemoryBudget(size_t bytes) {
  if (auto manager = std::atomic_load(&event_manager_)) {
    manager->UpdateMemoryBudget(bytes);
  }
}

void EventPipeline::EnsureAnnotator(
    const VideoContext& video_context,
    const std::vector<std::pair<Task, std::u8string>>& tasks,
    bool force_rebuild) {
  if (video_context.width <= 0 || video_context.height <= 0 ||
      video_context.format == AV_PIX_FMT_NONE) {
    return;
  }

  if (!force_rebuild) {
    std::lock_guard lock(annotator_mutex_);
    if (annotator_) {
      return;
    }
  }

  std::shared_ptr<DynamicAnnotator> candidate;
  try {
    candidate = std::make_shared<DynamicAnnotator>();

    DynamicAnnotator::LegendConfig legend_cfg;
    if (scheduler_) {
      const auto& conf = scheduler_->GetConfig();
      if (!conf.font_path.empty()) {
        legend_cfg.font_path = conf.font_path;
      }
      if (!conf.legend_position.empty()) {
        legend_cfg.SetLocation(conf.legend_position);
      }
      legend_cfg.prealloc_max_labels = conf.legend_prealloc_max_labels;
      legend_cfg.prealloc_max_label_len = conf.legend_prealloc_max_label_len;
    }

    if (const int ret = candidate->init(video_context, legend_cfg); ret < 0) {
      LOGE("Failed to initialize worker annotator ({}x{}, fmt={}): {}",
           video_context.width, video_context.height,
           static_cast<int>(video_context.format), ret);
      return;
    }
    candidate->update_legend(tasks);
  } catch (const std::exception& ex) {
    LOGE("Failed to initialize worker annotator: {}", ex.what());
    return;
  }

  // Publish the fully built annotator; the detect thread only ever sees
  // immutable published instances, so no long lock is needed on the hot path.
  std::lock_guard lock(annotator_mutex_);
  annotator_ = candidate;
  auto manager = std::atomic_load(&event_manager_);
  if (!manager) {
    manager = std::make_shared<CombinedEventManager>();
    std::atomic_store(&event_manager_, manager);
  }
  manager->SetAnnotator(std::move(candidate));
}

void EventPipeline::SubmitFrame(
    const AVFrame* frame, const ConvertorState& convertor,
    int64_t timestamp_ms,
    const std::vector<MonitorRegistry::Ptr>& monitors) {
  if (!frame) {
    return;
  }
  if (!convertor.convertor) {
    LOGW("frame submit skipped: convertor not initialized");
    if (metrics_) {
      metrics_->frames_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  // Frames decoded from a previous generation may still arrive right after a
  // reconnect; skip them rather than feeding a mismatched convertor.
  if (frame->width != convertor.width || frame->height != convertor.height ||
      frame->format != convertor.format) {
    if (metrics_) {
      metrics_->frames_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  if (monitors.empty()) {
    return;
  }

  // Admission control: only submit frames each detector actually wants, and
  // group monitors of the same task so they share one inference.
  std::vector<AdmissionEntry> entries;
  entries.reserve(monitors.size());
  std::unordered_map<unsigned long, MonitorRegistry::Ptr> by_id;
  by_id.reserve(monitors.size());
  for (const auto& ctx : monitors) {
    if (!ctx || !ctx->monitor || !ctx->analyzer || !ctx->detector) {
      continue;
    }
    if (!ctx->active.load(std::memory_order_acquire)) {
      continue;
    }
    const unsigned long analyzer_id = ctx->analyzer->id();
    entries.push_back(
        {analyzer_id, ctx->monitor->task, ctx->analyzer->WantsFrame()});
    by_id.emplace(analyzer_id, ctx);
  }
  if (entries.empty()) {
    return;
  }

  const auto admission = AdmitFrame(entries, static_cast<uint64_t>(timestamp_ms));
  if (admission.skipped > 0 && metrics_) {
    metrics_->frames_skipped.fetch_add(admission.skipped,
                                       std::memory_order_relaxed);
  }
  if (admission.groups.empty()) {
    return;
  }

  // FormatConvertor returns a view over a reusable internal buffer.
  // Convert and clone once per decoded frame; every admitted task group
  // shares the same read-only image.
  const cv::Mat img_view = (*convertor.convertor)(frame);
  auto img = std::make_shared<const cv::Mat>(img_view.clone());

  size_t submitted = 0;
  for (const auto& group : admission.groups) {
    for (const auto analyzer_id : group.analyzer_ids) {
      const auto it = by_id.find(analyzer_id);
      if (it == by_id.end()) {
        continue;
      }
      it->second->analyzer->AddVideoFrame(video_id_, timestamp_ms, img,
                                          group.shared);
      ++submitted;
    }
  }
  if (metrics_ && submitted > 0) {
    metrics_->frames_submitted.fetch_add(submitted, std::memory_order_relaxed);
  }
}

EventPipelineResult EventPipeline::ProcessFrame(
    const AVFrame* frame, int64_t timestamp_ms, int64_t best_effort_pts,
    const std::vector<MonitorRegistry::Ptr>& monitors) {
  EventPipelineResult result;
  std::vector<CombinedEventManager::DetectorFrameInfo> detector_infos;
  std::vector<MonitorEventSnapshot> snapshots;
  detector_infos.reserve(monitors.size());
  snapshots.reserve(monitors.size());

  for (const auto& ctx : monitors) {
    if (!ctx || !ctx->active.load(std::memory_order_acquire) ||
        !ctx->detector || !ctx->monitor) {
      continue;
    }
    const auto state = ctx->detector->ProcessFrame(timestamp_ms,
                                                   best_effort_pts);
    if (ctx->detector->detections().empty()) {
      continue;
    }
    detector_infos.emplace_back(
        ctx->monitor->code, ctx->monitor->task, state,
        ctx->detector->detections(), ctx->roi_entries,
        ctx->detector->event_start_timestamp(), ctx->detector->start_ts());
    snapshots.push_back(
        {ctx, ctx->monitor->code, state, ctx->detector->start_ts(),
         ctx->detector->timestamp(), ctx->detector->num_detection(),
         ctx->detector->event_duration(), ctx->monitor->levels,
         ctx->monitor->level_mode,
         ctx->current_highest_level.load(std::memory_order_relaxed)});
  }

  auto manager = std::atomic_load(&event_manager_);
  if (!manager) {
    return result;
  }

  auto combined = manager->ProcessFrame(frame, timestamp_ms, best_effort_pts,
                                        detector_infos);
  result.event_id = manager->CurrentEventId();
  result.transition = combined.transition;
  result.annotated_frame = combined.annotated_frame;
  result.flushed_event = std::move(combined.flushed_event);

  for (auto& snapshot : snapshots) {
    auto level_idx = ComputeLevelIndex(
        snapshot.levels, snapshot.level_mode, snapshot.num_detection,
        snapshot.event_duration, snapshot.monitor_code);

    switch (snapshot.state) {
      case EventDetector::kEventStart:
        if (level_idx < 0) {
          LOGT("monitor {} metric below first threshold {}",
               snapshot.monitor_code, snapshot.levels.first.front());
          level_idx = 0;
        }
        if (result.annotated_frame && !snapshot.levels.second.empty()) {
          snapshot.ctx->current_highest_level.store(level_idx);
          AlarmRequest request;
          request.monitor_code = snapshot.monitor_code;
          request.start_pts = snapshot.start_pts;
          request.event_pts = snapshot.event_pts;
          request.level_index = level_idx;
          request.level = snapshot.levels.second[level_idx];
          request.alarm_id = result.event_id;
          request.frame = result.annotated_frame;
          result.alarms.push_back(std::move(request));
        }
        break;
      case EventDetector::kEventActive:
        if (level_idx > snapshot.current_highest_level &&
            result.annotated_frame) {
          snapshot.ctx->current_highest_level.store(level_idx);
        }
        break;
      case EventDetector::kEventEnd:
        snapshot.ctx->current_highest_level.store(0);
        break;
      default:
        break;
    }
  }

  return result;
}

std::vector<std::unique_ptr<Event>> EventPipeline::FlushPending() {
  if (auto manager = std::atomic_load(&event_manager_)) {
    return manager->FlushPending();
  }
  return {};
}

int64_t EventPipeline::CurrentEventId() const {
  if (auto manager = std::atomic_load(&event_manager_)) {
    return manager->CurrentEventId();
  }
  return -1;
}

bool EventPipeline::HasActiveEvent() const {
  if (auto manager = std::atomic_load(&event_manager_)) {
    return manager->HasActiveEvent();
  }
  return false;
}

bool EventPipeline::HasAnnotator() const {
  std::lock_guard lock(annotator_mutex_);
  return annotator_ != nullptr;
}

int EventPipeline::ComputeLevelIndex(
    const std::pair<std::vector<int>, std::vector<std::string>>& levels,
    LevelMode level_mode, int64_t num_detection, int64_t event_duration,
    std::string_view monitor_code) {
  if (levels.first.empty() || levels.second.empty()) {
    // Detect thread only: apply a pending reset before touching the set.
    if (level_log_reset_.exchange(false, std::memory_order_relaxed)) {
      level_error_logged_.clear();
    }
    // Warn once per monitor per config generation: this is evaluated per
    // detected frame, and a bad levels config must not flood the log.
    if (level_error_logged_.emplace(monitor_code).second) {
      LOGE("monitor {} has invalid/empty levels configuration", monitor_code);
    }
    return 0;  // Always return lowest level (0) to avoid missing alarms
  }

  int64_t metric =
      level_mode == LevelMode::kNumPeople ? num_detection : event_duration;
  int idx = 0;
  const int total = static_cast<int>(levels.first.size());
  while (idx < total && levels.first[idx] <= metric) {
    ++idx;
  }
  --idx;
  idx = std::min(idx, static_cast<int>(levels.second.size()) - 1);
  return idx;
}

}  // namespace avs_analyzer
