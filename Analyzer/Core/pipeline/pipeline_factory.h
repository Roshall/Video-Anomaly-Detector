// Abstract Factory：一次产出"解码器 + 图像处理器 + 推理后端 + 编码器"这一族组件。
//
// 存在的意义：杜绝跨族组合（例如 NPU 解码配 CPU 预处理），
// 那会悄悄引入每帧 D2H，把硬件解码的收益全部吃回去。
#ifndef ANALYZER_PIPELINE_PIPELINE_FACTORY_H
#define ANALYZER_PIPELINE_PIPELINE_FACTORY_H

#include <memory>
#include <string>

#include "backend/inference_backend.h"
#include "imaging/frame.h"
#include "imaging/image_processor.h"
#include "imaging/pixel_format.h"

namespace avs_analyzer::pipeline {

using imaging::BackendKind;
using imaging::Frame;
using imaging::IImageProcessor;
using imaging::ModelSpec;
using imaging::Size;

// 推理类型来自 backend 命名空间：re-export 进来，
// 让工厂实现者无需来回写 backend:: 限定。
using backend::DetectionResult;
using backend::IInferenceBackend;

struct StreamDesc {
  std::string url;
  bool is_file{false};
};

struct EncodeDesc {
  Size size{};
  int fps{25};
  std::string url;
};

class IDecoder {
 public:
  virtual ~IDecoder() = default;
  [[nodiscard]] virtual BackendKind backend() const noexcept = 0;
  [[nodiscard]] virtual bool Open(const StreamDesc& desc) = 0;
};

class IEncoder {
 public:
  virtual ~IEncoder() = default;
  [[nodiscard]] virtual BackendKind backend() const noexcept = 0;
  virtual bool Encode(const Frame& frame) = 0;
  virtual void Flush() {}
};

class IPipelineFactory {
 public:
  virtual ~IPipelineFactory() = default;

  [[nodiscard]] virtual BackendKind backend() const noexcept = 0;

  [[nodiscard]] virtual std::unique_ptr<IDecoder> CreateDecoder(
      const StreamDesc& desc) = 0;

  [[nodiscard]] virtual std::unique_ptr<IImageProcessor>
  CreateImageProcessor() = 0;

  [[nodiscard]] virtual std::unique_ptr<backend::IInferenceBackend>
  CreateInference(const ModelSpec& spec, int device_id) = 0;

  [[nodiscard]] virtual std::unique_ptr<IEncoder> CreateEncoder(
      const EncodeDesc& desc) = 0;
};

}  // namespace avs_analyzer::pipeline

#endif
