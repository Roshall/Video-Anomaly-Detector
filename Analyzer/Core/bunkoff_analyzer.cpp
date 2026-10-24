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

  const auto roi = std::atomic_load(&roi_);
  if (!roi || roi->polygons.empty()) {
    return detection_results;
  }

  std::vector<bool> roi_has_overlap(roi->polygons.size(), false);

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
      for (size_t r = 0; r < roi->polygons.size(); ++r) {
        if (roi_has_overlap[r]) {
          continue;
        }
        std::vector<cv::Point2f> intersectionPoly;
        const auto interaction_area = cv::intersectConvexConvex(
            roi->polygons[r],
            std::vector<cv::Point>{{x, y}, {x2, y}, {x2, y2}},
            intersectionPoly);
        const auto min_area = std::min(roi->areas[r], bbox_area);
        if (min_area > 0.0 && (interaction_area / min_area) > overlap_ratio_threshold_) {
          roi_has_overlap[r] = true;
        }
      }
    }
  }

  // For each ROI with no overlapping bbox, emit its bbox.
  for (size_t r = 0; r < roi_has_overlap.size(); ++r) {
    if (!roi_has_overlap[r]) {
      detection_results.push_back(roi->bboxes[r]);
    }
  }

  return detection_results;
}

void BunkOffAnalyzer::SetROIs(const std::vector<std::vector<cv::Point>>& polygons) {
  auto state = std::make_shared<RoiState>();
  state->polygons = polygons;
  state->areas.reserve(polygons.size());
  state->bboxes.reserve(polygons.size());

  for (const auto& poly : polygons) {
    if (poly.empty()) {
      state->areas.push_back(0.0);
      state->bboxes.push_back(cv::Rect{});
      continue;
    }
    state->areas.push_back(cv::contourArea(poly));
    state->bboxes.push_back(computeBoundingBox(poly));
  }
  const std::shared_ptr<const RoiState> snapshot = state;
  std::atomic_store(&roi_, snapshot);
}

}  // namespace avs_analyzer
