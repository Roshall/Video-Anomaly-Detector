#include "analyzer.h"

#include <json/json.h>

#include <fstream>

#include "Utils/base64.h"
#include "Utils/img_processing.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "algorithm.h"
#include "config.h"
#include "control.h"
#include "detection_algorithm.h"
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

Analyzer::Analyzer(Monitor* monitor, const Config& config, float conf_th)
    : monitor_(monitor), conf_th_(conf_th) {
  if (monitor->level_mode == LevelMode::kNumPeople) {
    minimum_num_ = monitor->levels.first[0];
  }
  if (monitor->task == Task::kWandering) {
    algorithm_ = std::make_unique<TrackAlgorithm>(config);
  } else {
    algorithm_ = std::make_unique<DetectionAlgorithm>(config, monitor->task);
  }
}

std::vector<cv::Rect> Analyzer::ProcessVideoFrame(const cv::Mat& image) const {
  std::vector<cv::Rect> detection_results;
  Detection det = algorithm_->DetectObjects(image);
  if (monitor_->task == Task::kBunkOff) {
    if (!det.bbox.empty()) {
      bool work{false};
      for (size_t i = 0; i < det.confidences.size(); ++i) {
        if (det.confidences[i] < 0.3) {
          continue;
        }
        auto [x, y, w, h] = det.bbox[i];
        float m_x = static_cast<float >(w)/2 + static_cast<float >(x);
        auto m_y = static_cast<float >(y + h);

        auto point_to_polygon = cv::pointPolygonTest(monitor_->roi_points, {m_x, m_y}, false);
        if (point_to_polygon > 0) {
          work = true;
          break;
        }
      }
      if (not work) {
        detection_results.push_back(computeBoundingBox(monitor_->roi_points));
      }
    }
  } else if (!det.bbox.empty()) {
    for (size_t i = 0; i < det.confidences.size(); ++i) {
      if (det.confidences[i] < conf_th_) {
        continue;
      }
      auto [x, y, w, h] = det.bbox[i];
      float m_x = static_cast<float >(w)/2 + static_cast<float >(x);
      auto m_y = static_cast<float >(y + h);

      auto point_to_polygon = cv::pointPolygonTest(monitor_->roi_points, {m_x, m_y}, false);
      if (point_to_polygon > 0) {
        detection_results.push_back(det.bbox[i]);
      }
    }
    if (detection_results.size() < minimum_num_) {
      detection_results.clear();
    }
  }

  return detection_results;
}
}  // namespace avs_analyzer
