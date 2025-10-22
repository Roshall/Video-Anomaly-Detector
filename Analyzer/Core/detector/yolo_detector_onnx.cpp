//
// Created by lg on 10/11/25.
//
#include "yolo_detector_onnx.h"

namespace avs_analyzer {
YOLODetector::YOLODetector(const DL_INIT_PARAM& params)
    : params_(params), model_(std::make_unique<YOLOv8Detect>()) {
  model_->CreateSession(params);
}
std::vector<DL_RESULT> YOLODetector::Run(const cv::Mat& image) const {
  std::vector<DL_RESULT> res;
  {
    std::lock_guard lok(mutex_);
    res = model_->RunSession(image);
  }
  std::vector<DL_RESULT> filtered;
  auto tid = params_.target_id;
  for (auto& e : res) {
    if (e.classId == tid) {
      filtered.push_back(e);
    }
  }
  return filtered;
}
YOLODetector::YOLODetector(std::string_view mode_path, bool use_accelerator,
                           int target_id)
    : params_(kDefaultParam), model_(std::make_unique<YOLOv8Detect>()) {
  params_.modelPath = mode_path;
  params_.target_id = target_id;
  params_.cudaEnable = use_accelerator;
  model_->CreateSession(params_);
}
}