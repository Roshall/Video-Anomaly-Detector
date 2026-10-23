// Strategy：图像处理的抽象（缩放 / 像素格式转换 / letterbox / 归一化）。
//
// 两套实现：
//   - OpenCvImageProcessor（CPU）：cv::resize / cvtColor / NCHW 排布
//   - DvppImageProcessor（NPU）：hi_mpi_vpc_* + AIPP（Phase 6）
// 两者必须像素级等价，由 tests/imaging/image_processor_contract_test.cpp 保证。
//
// 解耦说明：图像引擎与推理引擎只通过中性契约 io/tensor.h 通信。图像引擎按
// ModelSpec 产出 ModelInput，至于谁来消费、跑在哪种推理硬件上，本模块不关心。
#ifndef ANALYZER_IMAGING_IMAGE_PROCESSOR_H
#define ANALYZER_IMAGING_IMAGE_PROCESSOR_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "frame.h"
#include "frame_storage.h"
#include "io/tensor.h"
#include "pixel_format.h"

namespace avs_analyzer::imaging {

// 张量与输入期望来自中性契约层；在成像命名空间 re-export，方便本域代码书写。
using io::DeviceTensor;
using io::HostTensor;
using io::ModelInput;
using io::ModelSpec;

enum class Op {
  kResize,
  kPixelFormatConvert,
  kLetterbox,
  kNormalize,
  kCrop,
};

class IImageProcessor {
 public:
  virtual ~IImageProcessor() = default;

  /// 展示 / 推流 / 抓图用：缩放到 out 并转成 fmt
  [[nodiscard]] virtual std::unique_ptr<Frame> ResizeAndConvert(
      const Frame& in, Size out, PixelFormat fmt) = 0;

  /// 推理用：letterbox 到 spec.input_size 并转成后端可直接消费的数据
  [[nodiscard]] virtual ModelInput PrepareModelInput(
      const Frame& in, const ModelSpec& spec) = 0;

  [[nodiscard]] virtual BackendKind backend() const noexcept = 0;
  [[nodiscard]] virtual bool Supports(Op op) const noexcept = 0;
};

}  // namespace avs_analyzer::imaging

#endif
