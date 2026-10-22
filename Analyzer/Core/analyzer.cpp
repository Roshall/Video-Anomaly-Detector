#include "analyzer.h"

#include <vector>

#include "Utils/log.h"

#include "algorithm.h"
#include "bunkoff_analyzer.h"
#include "config.h"
#include "detection_algorithm.h"
#include "general_analyzer.h"
#include "monitor.h"
#include "scheduler.h"
#include "track_algorithm.h"

namespace avs_analyzer {
Analyzer::Analyzer(Monitor* monitor, const Config& config)
    : task_(monitor->task),
      conf_th_(monitor->conf_threshold),
      result_timeout_(
          std::chrono::milliseconds(config.inference_result_timeout_ms)) {
  if (task_ == Task::kWandering) {
    algorithm_ = std::make_unique<TrackAlgorithm>(monitor->levels.first[0]);
  } else {
    algorithm_ = std::make_unique<DetectionAlgorithm>();
  }
}

Analyzer::~Analyzer() = default;

Detection Analyzer::FetchRawDetection() const {
  const auto ts_ms = WantsFrame();
  Detection det;
  while (!stopped_.load(std::memory_order_acquire)) {
    if (!algorithm_->TryProcessDetections(det, result_timeout_)) {
      if (metrics_) {
        metrics_->detect_wait_timeout.fetch_add(1, std::memory_order_relaxed);
      }
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
      auto last = last_timeout_log_ms_.load(std::memory_order_relaxed);
      if (now_ms - last >= 1000 &&
          last_timeout_log_ms_.compare_exchange_strong(last, now_ms,
                                                       std::memory_order_relaxed)) {
        LOGW("Analyzer {}: timed out after {} ms waiting for inference result "
             "for frame {}",
             id_, result_timeout_.count(), ts_ms);
      }
      det = Detection{};
      det.frame_ts = ts_ms;
      return det;
    }
    if (det.frame_ts >= ts_ms) {
      return det;
    }
    LOGT("Analyzer {}: skipping stale detections from frame {} (requested {})",
         id_, det.frame_ts, ts_ms);
  }
  det = Detection{};
  det.frame_ts = ts_ms;
  return det;
}

void Analyzer::AddVideoFrame(uint64_t video_id, uint64_t frame_id, ImagePtr image,
                             std::shared_ptr<SharedInference> shared) {
  add_task_callback_(id_, video_id, frame_id, std::move(image),
                     std::move(shared));
}

uint64_t Analyzer::NextId() {
  static std::atomic<uint64_t> global_id = 0;
  return global_id.fetch_add(1, std::memory_order_relaxed);
}

std::unique_ptr<Analyzer> CreateAnalyzer(Monitor* monitor, const Config& config) {
  if (!monitor) {
    return {};
  }
  if (monitor->task == Task::kBunkOff) {
    return std::make_unique<BunkOffAnalyzer>(monitor, config);
  }
  return std::make_unique<GeneralAnalyzer>(monitor, config);
}
}  // namespace avs_analyzer
