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
  const auto roi = std::atomic_load(&roi_);
  const bool has_roi = roi && !roi->points.empty();
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
        cv::pointPolygonTest(roi->points, {m_x, m_y}, false);
    if (point_to_polygon > 0) {
      detection_results.push_back(det.bbox[i]);
    }
  }

  return detection_results;
}

void GeneralAnalyzer::SetROIs(const std::vector<std::vector<cv::Point>>& polygons) {
  // General analyzer expects a single ROI polygon; use the first when present.
  if (polygons.empty() || polygons.front().empty()) {
    std::atomic_store(&roi_, std::shared_ptr<const RoiState>());
    return;
  }

  auto state = std::make_shared<RoiState>();
  state->points = polygons.front();
  state->area = cv::contourArea(state->points);
  state->bbox = cv::boundingRect(state->points);
  const std::shared_ptr<const RoiState> snapshot = state;
  std::atomic_store(&roi_, snapshot);
}

}  // namespace avs_analyzer
