#ifndef ANALYZER_GENERAL_ANALYZER_H
#define ANALYZER_GENERAL_ANALYZER_H

#include "analyzer.h"

#include <opencv2/opencv.hpp>
#include <vector>

namespace avs_analyzer {

class GeneralAnalyzer final : public Analyzer {
 public:
  GeneralAnalyzer(Monitor* monitor, const Config& config);
  ~GeneralAnalyzer() override;

  [[nodiscard]] std::vector<cv::Rect> RetrieveDetection() const override;
  void SetROIs(const std::vector<std::vector<cv::Point>>& polygons) override;

 private:
  double roi_area_{};
  cv::Rect roi_bbox_{};
  std::vector<cv::Point> roi_points_{};
  mutable std::atomic<bool> warned_no_roi_{false};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_GENERAL_ANALYZER_H
