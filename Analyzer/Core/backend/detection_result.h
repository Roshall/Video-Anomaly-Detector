// 兼容垫片：DetectionResult 已上移到中性契约层 io/detection_result.h。
//
// 推理后端与下游消费者长期使用 backend::DetectionResult；这里 re-export
// 回 avs_analyzer::backend。新代码建议直接用 avs_analyzer::io::DetectionResult。
//
// 迁移路径：inference.h 里的 DL_RESULT 最终会变成
//     using DL_RESULT = avs_analyzer::io::DetectionResult;
// 以便 DetectionAlgorithm 只依赖中性契约，而不绑定任何推理后端。
#ifndef ANALYZER_BACKEND_DETECTION_RESULT_H
#define ANALYZER_BACKEND_DETECTION_RESULT_H

#include "io/detection_result.h"

namespace avs_analyzer::backend {

using io::DetectionResult;

}  // namespace avs_analyzer::backend

#endif
