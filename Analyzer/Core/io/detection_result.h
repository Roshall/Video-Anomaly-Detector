// 检测结果的值类型。归"中性契约层"所有：下游（tracking / 事件状态机）只应
// 依赖这个纯值类型，而不必依赖任何具体推理后端。
//
// 依赖 OpenCV 的 cv::Rect 作为框类型，除此之外不依赖任何引擎或第三方。
#ifndef ANALYZER_IO_DETECTION_RESULT_H
#define ANALYZER_IO_DETECTION_RESULT_H

#include <opencv2/core.hpp>

namespace avs_analyzer::io {

struct DetectionResult {
  int class_id{0};
  float confidence{0.0f};
  cv::Rect box{};
};

}  // namespace avs_analyzer::io

#endif
