//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_YOLO_DETECTOR_ONNX_H
#define ANALYZER_YOLO_DETECTOR_ONNX_H
#include <memory>

#include "inference.h"
#include "algorithm.h"

namespace avs_analyzer {
constexpr DL_INIT_PARAM kDefaultParam{};

class YOLODetector {
 public:
  explicit YOLODetector(const DL_INIT_PARAM& params = kDefaultParam);
  explicit YOLODetector(std::string_view mode_path,
                        bool use_accelerator = false, int target_id = 0);

  [[nodiscard]] std::vector<DL_RESULT> Run(const cv::Mat& image) const;

 private:
  DL_INIT_PARAM params_;
  std::unique_ptr<YOLOv8Detect> model_;
  mutable std::mutex mutex_;
};
}

#endif  // ANALYZER_YOLO_DETECTOR_ONNX_H
