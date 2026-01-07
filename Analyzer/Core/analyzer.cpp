#include "analyzer.h"

#include <json/json.h>
#include <vector>

#include "Utils/log.h"

#include "algorithm.h"
#include "bunkoff_analyzer.h"
#include "detection_algorithm.h"
#include "general_analyzer.h"
#include "monitor.h"
#include "scheduler.h"
#include "track_algorithm.h"

namespace avs_analyzer {
Analyzer::Analyzer(Monitor* monitor, const Config& config)
    : task_(monitor->task),
      conf_th_(monitor->conf_threshold) {
  if (task_ == Task::kWandering) {
    algorithm_ = std::make_unique<TrackAlgorithm>(monitor->levels.first[0]);
  } else {
    algorithm_ = std::make_unique<DetectionAlgorithm>();
  }
}

Analyzer::~Analyzer() = default;

Detection Analyzer::FetchRawDetection() const {
  Detection det;
  const auto ts_ms = WantsFrame();
  while (true) {
    det = algorithm_->ProcessDetections();
    if (det.frame_ts >= ts_ms) {
      break;
    }
    LOGD("Analyzer {}: skipping stale detections from frame {} (requested {})",
         id_, det.frame_ts, ts_ms);
  }
  return det;
}

void Analyzer::AddVideoFrame(uint64_t video_id, uint64_t frame_id, ImagePtr image) {
  add_task_callback_(id_, video_id, frame_id, std::move(image));
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
