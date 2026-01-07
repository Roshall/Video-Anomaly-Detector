#include "bunkoff_analyzer.h"

#include <algorithm>

#include "algorithm.h"
#include "config.h"
#include "monitor.h"

namespace avs_analyzer {

static cv::Rect computeBoundingBox(const std::vector<cv::Point>& points) {
  if (points.empty()) {
    return cv::Rect{};
  }
  int minX = points[0].x, minY = points[0].y;
  int maxX = points[0].x, maxY = points[0].y;

  for (const auto& pt : points) {
    minX = std::min(minX, pt.x);
    minY = std::min(minY, pt.y);
    maxX = std::max(maxX, pt.x);
    maxY = std::max(maxY, pt.y);
  }

  return {minX, minY, maxX - minX, maxY - minY};
}

BunkOffAnalyzer::BunkOffAnalyzer(Monitor* monitor, const Config& config)
    : Analyzer(monitor, config) {
  // Clamp to [0, 1] for ratio comparisons.
  overlap_ratio_threshold_ = std::clamp(config.bunkoff_overlap_ratio, 0.0, 1.0);
}

BunkOffAnalyzer::~BunkOffAnalyzer() = default;

std::vector<cv::Rect> BunkOffAnalyzer::RetrieveDetection() const {
  std::vector<cv::Rect> detection_results;
  const Detection det = FetchRawDetection();

  if (roi_polygons_.empty()) {
    return detection_results;
  }

  std::vector<bool> roi_has_overlap(roi_polygons_.size(), false);

  if (!det.bbox.empty()) {
    for (size_t i = 0; i < det.confidences.size(); ++i) {
      if (det.confidences[i] < conf_threshold()) {
        continue;
      }

      const auto [x, y, w, h] = det.bbox[i];
      const auto x2 = x + w;
      const auto y2 = y + h;
      const double bbox_area = static_cast<double>(w) * static_cast<double>(h);

      // Check this bbox against each ROI polygon.
      for (size_t r = 0; r < roi_polygons_.size(); ++r) {
        if (roi_has_overlap[r]) {
          continue;
        }
        std::vector<cv::Point2f> intersectionPoly;
        const auto interaction_area = cv::intersectConvexConvex(
            roi_polygons_[r],
            std::vector<cv::Point>{{x, y}, {x2, y}, {x2, y2}},
            intersectionPoly);
        const auto min_area = std::min(roi_areas_[r], bbox_area);
        if (min_area > 0.0 && (interaction_area / min_area) > overlap_ratio_threshold_) {
          roi_has_overlap[r] = true;
        }
      }
    }
  }

  // For each ROI with no overlapping bbox, emit its bbox.
  for (size_t r = 0; r < roi_has_overlap.size(); ++r) {
    if (!roi_has_overlap[r]) {
      detection_results.push_back(roi_bboxes_[r]);
    }
  }

  return detection_results;
}

void BunkOffAnalyzer::SetROIs(const std::vector<std::vector<cv::Point>>& polygons) {
  roi_polygons_ = polygons;

  roi_areas_.clear();
  roi_bboxes_.clear();
  roi_areas_.reserve(roi_polygons_.size());
  roi_bboxes_.reserve(roi_polygons_.size());

  for (const auto& poly : roi_polygons_) {
    if (poly.empty()) {
      roi_areas_.push_back(0.0);
      roi_bboxes_.push_back(cv::Rect{});
      continue;
    }
    roi_areas_.push_back(cv::contourArea(poly));
    roi_bboxes_.push_back(computeBoundingBox(poly));
  }
}

}  // namespace avs_analyzer
