#include "pipeline/pipeline_factories.h"

#include <utility>

#include "Utils/log.h"
#include "backend/onnx_inference_backend.h"

namespace avs_analyzer::pipeline {

// ---------------------------------------------------------------- CPU 预设

std::unique_ptr<IDecoder> CpuPipelineFactory::CreateDecoder(const StreamDesc&) {
  return std::make_unique<FfmpegDecoder>();
}

std::unique_ptr<IImageProcessor> CpuPipelineFactory::CreateImageProcessor() {
  return std::make_unique<imaging::OpenCvImageProcessor>();
}

std::unique_ptr<backend::IInferenceBackend> CpuPipelineFactory::CreateInference(
    const ModelSpec& spec, int) {
  DL_INIT_PARAM params{};
  params.imgSize = {spec.input_size.w, spec.input_size.h};
  // 色域与归一化已由图像引擎按 ModelSpec 完成（OpenCvImageProcessor），
  // 所以这里只负责加载模型与执行，不再触碰像素。
  // modelPath 为空时后端保持未就绪，Run() 安全返回空。
  return std::make_unique<backend::OnnxInferenceBackend>(params);
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
