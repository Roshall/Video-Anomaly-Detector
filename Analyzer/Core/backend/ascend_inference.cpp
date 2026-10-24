#include "backend/ascend_inference.h"

#include <variant>

#include "Utils/log.h"

namespace avs_analyzer::backend {

AscendInferenceBackend::AscendInferenceBackend(ascend::OmModelConfig model,
                                               int device_id)
    : model_cfg_(std::move(model)), device_id_(device_id) {
  if (ascend::AclRuntime::Instance().Initialize() != ascend::AscendError::kSuccess) {
    last_error_ = ascend::AscendError::kSdkUnavailable;
    LOGW("Ascend: 运行时不可用，推理后端保持未就绪（device={}）", device_id_);
    return;
  }

  model_ = std::make_unique<ascend::OmModel>(device_id_, model_cfg_);
  if (model_->Load() != ascend::AscendError::kSuccess) {
    last_error_ = ascend::AscendError::kModelLoadFailed;
    LOGW("Ascend: .om 模型加载失败 path={}", model_cfg_.model_path);
    model_.reset();
    return;
  }

  ready_ = true;
  last_error_ = ascend::AscendError::kSuccess;
}

AscendInferenceBackend::~AscendInferenceBackend() = default;

std::vector<DetectionResult> AscendInferenceBackend::Run(
    const ModelInput& input) {
  if (!ready_) {
    LOGW("Ascend: 推理后端未就绪（{}），返回空结果",
         ascend::AscendErrorToString(last_error_));
    return {};
  }

  if (std::holds_alternative<DeviceTensor>(input)) {
    const auto& dt = std::get<DeviceTensor>(input);
    if (dt.ptr == nullptr || dt.bytes == 0) {
      LOGW("Ascend: 收到空 device 输入，跳过推理");
      return {};
    }
    // TODO(硬件联调)：aclmdlExecute(model_->model_id(), input_dataset, output_dataset)。
    // 零拷贝的关键就在这里：dt.ptr 直接挂成输入 dataset 的 data buffer，
    // 不做任何 D2H；归一化与色域转换由动态 AIPP 在推理节点内完成——
    // 这正是 io::DeviceTensor 不预乘 1/255 的原因（见 io/tensor.h）。
    LOGW("Ascend: 零拷贝推理待接入（device={} {}x{} {} 字节）", device_id_,
         dt.size.w, dt.size.h, dt.bytes);
    return {};
  }

  const auto& ht = std::get<HostTensor>(input);
  // TODO(硬件联调)：Host NCHW → aclrtMemcpy(H2D) → aclmdlExecute。
  // 这条路只在"CPU 预处理 + NPU 推理"的混合作业里出现，正常应该走 DeviceTensor。
  LOGW("Ascend: host 输入推理待接入（{} 个 float）", ht.data.size());
  return {};
}

}  // namespace avs_analyzer::backend
