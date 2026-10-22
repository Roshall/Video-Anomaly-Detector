//
// Created by lg on 10/11/25.
//

#include "detection_algorithm.h"

#include "detector/inference.h"

namespace avs_analyzer {
bool DetectionAlgorithm::TryProcessDetections(
    Detection& out, std::chrono::milliseconds timeout) {
  QueueValue value;
  if (!FetchOneFrameResults(value, timeout)) {
    return false;
  }
  auto& [res, frame_ms] = value;
  std::vector<cv::Rect> bbox;
  std::vector<float> confs;
  bbox.reserve(res.size());
  confs.reserve(res.size());
  for (auto& obj : res) {
    bbox.push_back(obj.box);
    confs.push_back(obj.confidence);
  }
  out = {std::move(bbox), std::move(confs), frame_ms};
  return true;
}
}  // namespace avs_analyzer