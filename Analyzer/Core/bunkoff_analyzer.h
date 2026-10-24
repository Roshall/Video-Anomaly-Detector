#ifndef ANALYZER_BUNKOFF_ANALYZER_H
#define ANALYZER_BUNKOFF_ANALYZER_H

#include "analyzer.h"

#include <memory>

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

  // Immutable ROI state; SetROIs swaps in a new snapshot (pull thread) while
  // RetrieveDetection reads one (detect thread).
  struct RoiState {
    std::vector<std::vector<cv::Point>> polygons;
    std::vector<double> areas;
    std::vector<cv::Rect> bboxes;
  };
  std::shared_ptr<const RoiState> roi_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_BUNKOFF_ANALYZER_H
