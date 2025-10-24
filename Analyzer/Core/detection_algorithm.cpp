//
// Created by lg on 10/11/25.
//

#include "detection_algorithm.h"

#include "detector/constant_yolo.h"
#include "detector/yolo_detector_onnx.h"
#include "detector/inference.h"
#include "task.h"
#include "config.h"

namespace avs_analyzer {
DetectionAlgorithm::DetectionAlgorithm(const Config& config, Task task)
    :detector_(GetDetector(config.model_dir, task, config.use_accelerator)) {
}
Detection DetectionAlgorithm::DetectObjects(const cv::Mat& image) {
  std::vector<cv::Rect> bbox;
  std::vector<float> confs;
  for (auto& obj : detector_->Run(image)) {
    bbox.push_back(obj.box);
    confs.push_back(obj.confidence);
  }
  return {bbox, confs};
}
}  // namespace avs_analyzer