// Adapter：把 ONNX Runtime 推理包装成 backend::IInferenceBackend 的形状。
//
// 解耦的核心：本类**不做任何像素处理**。letterbox / 色域转换 / 归一化都归属
// 图像引擎（imaging::OpenCvImageProcessor 或 imaging::DvppImageProcessor），
// 它们按 io::ModelSpec 产出 io::ModelInput；本类只负责把张量喂给模型并解出
// 检测结果。于是"预处理跑在 CPU 还是 NPU"变成一个组合问题，不再需要改动
// 推理引擎。
//
// 输入契约（io::ModelInput 的两个分支）：
//   - HostTensor（NCHW float32）  → CPU 预处理的产物，直接跑
//   - DeviceTensor（NV12，NPU 上）→ CPU 后端没有设备上下文，取不到像素，
//                                   只能优雅返回空（由 NPU 后端处理）
//
// 分层：只依赖中性契约 io/ 与 detector/inference_types.h，不 include 图像
// 处理引擎，由 tests/architecture/layering_test.cpp 强制。ORT 头用 pimpl
// 隔离在 .cpp 内，避免污染下游编译单元。
#ifndef ANALYZER_BACKEND_ONNX_INFERENCE_BACKEND_H
#define ANALYZER_BACKEND_ONNX_INFERENCE_BACKEND_H

#include <memory>
#include <vector>

#include "backend/inference_backend.h"
#include "detector/inference_types.h"
#include "io/tensor.h"

namespace avs_analyzer::backend {

using io::DeviceTensor;
using io::HostTensor;

class OnnxInferenceBackend final : public IInferenceBackend {
 public:
  /// 未加载模型：用于测试与探针，Run() 一律返回空。
  OnnxInferenceBackend();
  /// 加载 .onnx。失败（文件缺失 / ORT 报错）时保持未就绪并记一条 LOGW，
  /// 不抛出——调用方无需为"模型没准备好"写 try/catch。
  explicit OnnxInferenceBackend(DL_INIT_PARAM params);
  ~OnnxInferenceBackend() override;

  [[nodiscard]] BackendKind backend() const noexcept override {
    return BackendKind::kCpu;
  }

  [[nodiscard]] std::vector<DetectionResult> Run(const ModelInput& input) override;

  /// 模型是否加载成功。
  [[nodiscard]] bool ready() const noexcept { return ready_; }

 private:
  struct Impl;  // 隔离 ORT 头
  std::unique_ptr<Impl> impl_;
  bool ready_{false};
};

/// 测试用工厂：产出一个未加载 .onnx 的 CPU 后端，用来验证类型契约与降级路径。
[[nodiscard]] std::unique_ptr<IInferenceBackend>
CreateOnnxInferenceBackendForTest();

}  // namespace avs_analyzer::backend

#endif  // ANALYZER_BACKEND_ONNX_INFERENCE_BACKEND_H
