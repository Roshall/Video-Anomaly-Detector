// IPipelineFactory 的两个具体预设：一次产出同族的"解码 + 预处理 + 推理 + 编码"。
//
// 抽象工厂的意义在于杜绝跨族组合——例如 NPU 解码配 CPU 预处理，会悄悄引入
// 每帧 D2H，把硬件解码的收益全部吃回去。所以这里只暴露整族预设，
// 不提供"挑单个组件"的入口。
#ifndef ANALYZER_PIPELINE_PIPELINE_FACTORIES_H
#define ANALYZER_PIPELINE_PIPELINE_FACTORIES_H

#include <memory>

#include "ascend/ascend_types.h"
#include "backend/ascend_inference.h"
#include "imaging/dvpp_image_processor.h"
#include "imaging/opencv_image_processor.h"
#include "pipeline/ascend_decoder.h"
#include "pipeline/ffmpeg_decoder.h"
#include "pipeline/null_encoder.h"
#include "pipeline/pipeline_factory.h"

namespace avs_analyzer::pipeline {

/// CPU 预设：FFmpeg 软解 + OpenCV 预处理 + ONNX 推理 + 空编码器。
///
/// 注意预处理与推理是**两个独立组件**：前者按 ModelSpec 产出 io::HostTensor，
/// 后者只消费张量。想把预处理换到 NPU 上时，只需换这一件，推理不动。
class CpuPipelineFactory final : public IPipelineFactory {
 public:
  [[nodiscard]] BackendKind backend() const noexcept override {
    return BackendKind::kCpu;
  }

  [[nodiscard]] std::unique_ptr<IDecoder> CreateDecoder(
      const StreamDesc& desc) override;

  [[nodiscard]] std::unique_ptr<IImageProcessor> CreateImageProcessor() override;

  [[nodiscard]] std::unique_ptr<backend::IInferenceBackend> CreateInference(
      const ModelSpec& spec, int device_id) override;

  [[nodiscard]] std::unique_ptr<IEncoder> CreateEncoder(
      const EncodeDesc& desc) override;
};

/// Ascend 预设：VDEC 硬解 + VPC 预处理 + ACL 推理，全部落在同一 NPU 上。
class AscendPipelineFactory final : public IPipelineFactory {
 public:
  explicit AscendPipelineFactory(int device_id = 0);
  AscendPipelineFactory(int device_id, ascend::VdecChannelConfig vdec,
                        ascend::OmModelConfig model);

  [[nodiscard]] BackendKind backend() const noexcept override {
    return BackendKind::kNpu;
  }

  [[nodiscard]] std::unique_ptr<IDecoder> CreateDecoder(
      const StreamDesc& desc) override;

  [[nodiscard]] std::unique_ptr<IImageProcessor> CreateImageProcessor() override;

  [[nodiscard]] std::unique_ptr<backend::IInferenceBackend> CreateInference(
      const ModelSpec& spec, int device_id) override;

  [[nodiscard]] std::unique_ptr<IEncoder> CreateEncoder(
      const EncodeDesc& desc) override;

 private:
  int device_id_{0};
  ascend::VdecChannelConfig vdec_cfg_{};
  ascend::OmModelConfig model_cfg_{};
};

/// 按后端种类建工厂。kNpu 时若 CANN 不可用，组件会在 Open() 阶段失败，
/// 调用方据此回落 CPU（与迁移计划的"自动回落"一致）。
[[nodiscard]] std::unique_ptr<IPipelineFactory> CreatePipelineFactory(
    BackendKind kind, int device_id = 0);

}  // namespace avs_analyzer::pipeline

#endif
