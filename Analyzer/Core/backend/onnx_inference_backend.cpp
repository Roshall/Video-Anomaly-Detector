#include "backend/onnx_inference_backend.h"

#include <exception>
#include <variant>

#include "Utils/log.h"
#include "detector/inference.h"

namespace avs_analyzer::backend {

struct OnnxInferenceBackend::Impl {
  YOLOv8Detect model;
};

OnnxInferenceBackend::OnnxInferenceBackend() : impl_(std::make_unique<Impl>()) {}

OnnxInferenceBackend::OnnxInferenceBackend(DL_INIT_PARAM params)
    : impl_(std::make_unique<Impl>()) {
  if (params.modelPath.empty()) {
    LOGW("OnnxInferenceBackend: 未提供 modelPath，后端未就绪（Run 将返回空）");
    return;
  }
  try {
    const char* rc = impl_->model.CreateSession(params);
    ready_ = (rc != nullptr);
    if (!ready_) {
      LOGW("OnnxInferenceBackend: 模型加载未返回 OK");
    }
  } catch (const std::exception& e) {
    LOGW("OnnxInferenceBackend: 加载模型失败: {}", e.what());
    ready_ = false;
  }
}

OnnxInferenceBackend::~OnnxInferenceBackend() = default;

std::vector<DetectionResult> OnnxInferenceBackend::Run(const ModelInput& input) {
  if (!ready_) {
    return {};
  }

  // CPU 后端只认 host 张量。DeviceTensor 是 NPU 预处理（DVPP + AIPP）的产物，
  // 像素留在设备上，CPU 侧没有可解引用的地址，因此这里优雅返回空而不是
  // 崩溃——这类输入应由 AscendInferenceBackend 承接。
  if (!std::holds_alternative<HostTensor>(input)) {
    LOGW("OnnxInferenceBackend: 收到 DeviceTensor，CPU 后端无法直接消费");
    return {};
  }

  const auto& tensor = std::get<HostTensor>(input);
  if (tensor.data.empty()) {
    return {};
  }

  const YOLOv8Detect::PreprocInfo pp{tensor.letterbox.scale,
                                     tensor.letterbox.pad_x,
                                     tensor.letterbox.pad_y};
  const auto raw = impl_->model.RunSession(tensor, pp);

  std::vector<DetectionResult> out;
  out.reserve(raw.size());
  for (const auto& r : raw) {
    out.push_back(DetectionResult{r.classId, r.confidence, r.box});
  }
  return out;
}

std::unique_ptr<IInferenceBackend> CreateOnnxInferenceBackendForTest() {
  return std::make_unique<OnnxInferenceBackend>();
}

}  // namespace avs_analyzer::backend
