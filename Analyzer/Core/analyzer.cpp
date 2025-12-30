#include "analyzer.h"

#include <json/json.h>
#include <vector>

#include "Utils/log.h"

#include "algorithm.h"
#include "detection_algorithm.h"
#include "monitor.h"
#include "scheduler.h"
#include "track_algorithm.h"

namespace avs_analyzer {
cv::Rect computeBoundingBox(const std::vector<cv::Point>& points) {
  // Initialize the min and max coordinates with the first point.
  int minX = points[0].x, minY = points[0].y;
  int maxX = points[0].x, maxY = points[0].y;

  // Iterate through all the points to find the min and max x and y values.
  for (const auto& pt : points) {
    minX = std::min(minX, pt.x);
    minY = std::min(minY, pt.y);
    maxX = std::max(maxX, pt.x);
    maxY = std::max(maxY, pt.y);
  }

  // Return the bounding box as a cv::Rect.
  return {minX, minY, maxX - minX, maxY - minY};
}

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

[[nodiscard]] std::vector<cv::Rect> Analyzer::RetrieveDetection(uint64_t ts_ms) const {
  frame_ts_ms_.store(ts_ms, std::memory_order_release);
  std::vector<cv::Rect> detection_results;
  Detection det;
  while (true) {
    det = algorithm_->ProcessDetections();
    if (det.frame_ts >= ts_ms) break;
    LOGD("Analyzer {}: skipping stale detections from frame {} (requested {})",
         id_, det.frame_ts, ts_ms);
  }

  if (task_ == Task::kBunkOff) {
    bool work{false};
    if (!det.bbox.empty()) {
      for (size_t i = 0; i < det.confidences.size(); ++i) {
        if (det.confidences[i] < conf_th_) {
          continue;
        }
        auto [x, y, w, h] = det.bbox[i];
        std::vector<cv::Point2f> intersectionPoly;
        auto x2 = x + w;
        auto y2 = y + h;
        auto interaction_area = cv::intersectConvexConvex(
            roi_points_,
            std::vector<cv::Point>{{x, y}, {x2, y}, {x2, y2}},
            intersectionPoly);
        auto min_area = std::min(roi_area_, static_cast<double>(w * h));
        if (interaction_area / min_area > 0.3) {
          work = true;
          break;
        }
      }
    }
    if (not work) {
      detection_results.push_back(roi_bbox_);
    }
  } else if (!det.bbox.empty()) {
    for (size_t i = 0; i < det.confidences.size(); ++i) {
      if (det.confidences[i] < conf_th_) {
        continue;
      }
      auto [x, y, w, h] = det.bbox[i];
      float m_x = static_cast<float>(w) / 2 + static_cast<float>(x);
      auto m_y = static_cast<float>(y + h);

      auto point_to_polygon =
          cv::pointPolygonTest(roi_points_, {m_x, m_y}, false);
      if (point_to_polygon > 0) {
        detection_results.push_back(det.bbox[i]);
      }
    }
  }

  return detection_results;
}

void Analyzer::AddVideoFrame(uint64_t video_id, uint64_t frame_id, ImagePtr image) {
  add_task_callback_(id_, video_id, frame_id, std::move(image));
}

uint64_t Analyzer::NextId() {
  static std::atomic<uint64_t> global_id = 0;
  return global_id.fetch_add(1, std::memory_order_relaxed);
}

void Analyzer::SetROI(std::vector<cv::Point>& points) {
  roi_area_ = cv::contourArea(points);
  roi_bbox_ = computeBoundingBox(points);
  roi_points_ = points;

}
}  // namespace avs_analyzer
