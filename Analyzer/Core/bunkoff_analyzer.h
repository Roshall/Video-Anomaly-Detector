#ifndef ANALYZER_BUNKOFF_ANALYZER_H
#define ANALYZER_BUNKOFF_ANALYZER_H

#include "analyzer.h"

#include <opencv2/opencv.hpp>
#include <vector>

namespace avs_analyzer {

class BunkOffAnalyzer final : public Analyzer {
 public:
  BunkOffAnalyzer(Monitor* monitor, const Config& config);
  ~BunkOffAnalyzer() override;

  [[nodiscard]] std::vector<cv::Rect> RetrieveDetection() const override;
  void SetROIs(const std::vector<std::vector<cv::Point>>& polygons) override;

 private:
  double overlap_ratio_threshold_{0.3};

  std::vector<std::vector<cv::Point>> roi_polygons_{};
  std::vector<double> roi_areas_{};
  std::vector<cv::Rect> roi_bboxes_{};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_BUNKOFF_ANALYZER_H
