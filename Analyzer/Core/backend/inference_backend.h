// Adapter 的目标接口：把 ORT 与 ACL（以及未来更多平台/NPU 的运行时）统一成
// 同一个形状，让 DetectionAlgorithm 不需要知道自己跑在哪种推理硬件上。
//
// 解耦说明：推理引擎只依赖中性契约层 io/（张量、输入期望、检测结果），
// 不再 include 图像处理引擎。谁来产出 ModelInput、像素在哪，本模块不关心。
// 由 tests/architecture/layering_test.cpp 强制 backend 不得 include imaging。
#ifndef ANALYZER_BACKEND_INFERENCE_BACKEND_H
#define ANALYZER_BACKEND_INFERENCE_BACKEND_H

#include <vector>

#include "io/common.h"
#include "io/detection_result.h"
#include "io/tensor.h"

namespace avs_analyzer::backend {

using io::BackendKind;
using io::DetectionResult;
using io::ModelInput;
using io::ModelSpec;

class IInferenceBackend {
 public:
  virtual ~IInferenceBackend() = default;

  [[nodiscard]] virtual BackendKind backend() const noexcept = 0;

  [[nodiscard]] virtual std::vector<DetectionResult> Run(
      const ModelInput& input) = 0;
};

}  // namespace avs_analyzer::backend

#endif
