//
// Created by lg on 10/11/25.
//

#include "detection_algorithm.h"

#include "detector/inference.h"

namespace avs_analyzer {
Detection DetectionAlgorithm::ProcessDetections() {
  std::vector<cv::Rect> bbox;
  std::vector<float> confs;
  auto [res, frame_ms] = FetchOneFrameResults();
  for (auto& obj : res) {
    bbox.push_back(obj.box);
    confs.push_back(obj.confidence);
  }
  return {bbox, confs};
}
}  // namespace avs_analyzer