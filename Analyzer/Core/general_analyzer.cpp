#include "general_analyzer.h"

#include "Utils/log.h"
#include "algorithm.h"
#include "monitor.h"

namespace avs_analyzer {

GeneralAnalyzer::GeneralAnalyzer(Monitor* monitor, const Config& config)
    : Analyzer(monitor, config) {}

GeneralAnalyzer::~GeneralAnalyzer() = default;

std::vector<cv::Rect> GeneralAnalyzer::RetrieveDetection() const {
  std::vector<cv::Rect> detection_results;
  const Detection det = FetchRawDetection();

  if (det.bbox.empty()) {
    return detection_results;
  }

  // If no ROI was configured, fall back to confidence-only filtering.
  const bool has_roi = !roi_points_.empty();
  if (!has_roi) {
    // Warn once per analyzer instance to avoid log spam
    bool expected = false;
    if (warned_no_roi_.compare_exchange_strong(expected, true)) {
      LOGW("GeneralAnalyzer {}: no ROI configured; using confidence-only filtering", id());
    }
  }

  for (size_t i = 0; i < det.confidences.size(); ++i) {
    if (det.confidences[i] < conf_threshold()) {
      continue;
    }
    if (!has_roi) {
      detection_results.push_back(det.bbox[i]);
      continue;
    }

    const auto [x, y, w, h] = det.bbox[i];
    const float m_x = static_cast<float>(w) / 2 + static_cast<float>(x);
    const float m_y = static_cast<float>(y + h);

    const auto point_to_polygon =
        cv::pointPolygonTest(roi_points_, {m_x, m_y}, false);
    if (point_to_polygon > 0) {
      detection_results.push_back(det.bbox[i]);
    }
  }

  return detection_results;
}

void GeneralAnalyzer::SetROIs(const std::vector<std::vector<cv::Point>>& polygons) {
  // General analyzer expects a single ROI polygon; use the first when present.
  if (polygons.empty() || polygons.front().empty()) {
    roi_points_.clear();
    roi_area_ = 0.0;
    roi_bbox_ = cv::Rect{};
    return;
  }

  roi_points_ = polygons.front();
  roi_area_ = cv::contourArea(roi_points_);
  roi_bbox_ = cv::boundingRect(roi_points_);
}

}  // namespace avs_analyzer
