// Adapter：把 Ascend ACL 推理包装成 backend::IInferenceBackend 的形状。
//
// 输入契约（io::ModelInput 的两个分支）：
//   - DeviceTensor（NV12，留在 NPU 上）→ 零拷贝直送模型，归一化交给动态 AIPP
//   - HostTensor（NCHW float32）       → 拷到设备后再跑（CPU 预处理时的回落路径）
//
// 分层：只依赖中性契约 io/ 与平台层 ascend/，不 include 图像处理引擎，
// 由 tests/architecture/layering_test.cpp 强制。
#ifndef ANALYZER_BACKEND_ASCEND_INFERENCE_H
#define ANALYZER_BACKEND_ASCEND_INFERENCE_H

#include <memory>
#include <vector>

#include "ascend/acl_runtime.h"
#include "ascend/ascend_types.h"
#include "backend/inference_backend.h"
#include "io/tensor.h"

namespace avs_analyzer::backend {

using io::DeviceTensor;
using io::HostTensor;

class AscendInferenceBackend final : public IInferenceBackend {
 public:
  explicit AscendInferenceBackend(ascend::OmModelConfig model,
                                  int device_id = 0);
  ~AscendInferenceBackend() override;

  [[nodiscard]] BackendKind backend() const noexcept override {
    return BackendKind::kNpu;
  }

  [[nodiscard]] std::vector<DetectionResult> Run(const ModelInput& input) override;

  /// 模型是否加载成功。未就绪时 Run() 返回空结果而不是崩溃。
  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] ascend::AscendError last_error() const noexcept {
    return last_error_;
  }

 private:
  ascend::OmModelConfig model_cfg_;
  int device_id_{0};
  std::unique_ptr<ascend::OmModel> model_;
  bool ready_{false};
  ascend::AscendError last_error_{ascend::AscendError::kNotInitialized};
};

}  // namespace avs_analyzer::backend

#endif
