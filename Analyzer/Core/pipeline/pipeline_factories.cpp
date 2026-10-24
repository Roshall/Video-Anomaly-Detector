#include "pipeline/pipeline_factories.h"

#include <utility>

#include "Utils/log.h"

namespace avs_analyzer::pipeline {

// ---------------------------------------------------------------- CPU 预设

std::unique_ptr<IDecoder> CpuPipelineFactory::CreateDecoder(const StreamDesc&) {
  return std::make_unique<FfmpegDecoder>();
}

std::unique_ptr<IImageProcessor> CpuPipelineFactory::CreateImageProcessor() {
  return std::make_unique<imaging::OpenCvImageProcessor>();
}

std::unique_ptr<backend::IInferenceBackend> CpuPipelineFactory::CreateInference(
    const ModelSpec&, int) {
  // 待办（不在此 PR 硬塞）：现有 YOLOv8Detect 把 letterbox + HWC→NCHW 预处理
  // 做在 RunSession(const cv::Mat&) 内部，而 IInferenceBackend 的契约是
  // "输入已经是预处理好的张量"。直接包一层会导致二次 letterbox，
  // 必须先把它外置成张量输入（OpenCvImageProcessor 已具备该能力）再接入。
  LOGW("CPU 推理后端（ONNX）待接入：需先外置 YOLOv8Detect 的预处理");
  return nullptr;
}

std::unique_ptr<IEncoder> CpuPipelineFactory::CreateEncoder(const EncodeDesc&) {
  return std::make_unique<NullEncoder>();
}

// ---------------------------------------------------------------- Ascend 预设

AscendPipelineFactory::AscendPipelineFactory(int device_id)
    : device_id_(device_id) {}

AscendPipelineFactory::AscendPipelineFactory(int device_id,
                                             ascend::VdecChannelConfig vdec,
                                             ascend::OmModelConfig model)
    : device_id_(device_id),
      vdec_cfg_(std::move(vdec)),
      model_cfg_(std::move(model)) {}

std::unique_ptr<IDecoder> AscendPipelineFactory::CreateDecoder(
    const StreamDesc&) {
  auto cfg = vdec_cfg_;
  cfg.device_id = device_id_;
  return std::make_unique<AscendDecoder>(cfg);
}

std::unique_ptr<IImageProcessor>
AscendPipelineFactory::CreateImageProcessor() {
  return std::make_unique<imaging::DvppImageProcessor>(device_id_);
}

std::unique_ptr<backend::IInferenceBackend>
AscendPipelineFactory::CreateInference(const ModelSpec& spec, int device_id) {
  auto cfg = model_cfg_;
  cfg.input = spec.input_size;
  // 归一化/色域转换下沉到 AIPP，所以这里恒开动态 AIPP，
  // 也让 DeviceTensor 不预乘 1/255 的约定成立。
  cfg.use_dynamic_aipp = true;
  return std::make_unique<backend::AscendInferenceBackend>(
      cfg, device_id >= 0 ? device_id : device_id_);
}

std::unique_ptr<IEncoder> AscendPipelineFactory::CreateEncoder(
    const EncodeDesc&) {
  // TODO(硬件联调)：VENC 硬编码。未接入前用 NullEncoder，保证链路完整。
  return std::make_unique<NullEncoder>();
}

std::unique_ptr<IPipelineFactory> CreatePipelineFactory(BackendKind kind,
                                                        int device_id) {
  if (kind == BackendKind::kNpu) {
    return std::make_unique<AscendPipelineFactory>(device_id);
  }
  return std::make_unique<CpuPipelineFactory>();
}

}  // namespace avs_analyzer::pipeline
