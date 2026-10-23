// 推理 I/O 契约：模型输入张量与输入期望。
//
// 这是"图像处理引擎"与"推理引擎"之间唯一的通信格式，归双方共有、任何一方
// 所有。图像引擎按 ModelSpec 产出 ModelInput；推理引擎消费 ModelInput 产出
// DetectionResult。因为它是中性类型，换掉任一引擎都不影响另一方。
#ifndef ANALYZER_IO_TENSOR_H
#define ANALYZER_IO_TENSOR_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "common.h"

namespace avs_analyzer::io {

/// 推理引擎对输入的期望（谁要跑模型谁声明）。
struct ModelSpec {
  Size input_size{640, 640};
  PixelFormat input_format{PixelFormat::kRgb24};  // 送进模型前的排布
  bool normalize{true};                           // 是否 /255
  bool planar_nchw{true};                         // 是否排成 NCHW
};

/// Host 侧张量：NCHW float32（CPU / OpenCvImageProcessor 的产物）
struct HostTensor {
  std::vector<float> data;
  std::array<std::int64_t, 4> dims{1, 3, 0, 0};
};

/// Device 侧缓冲：NV12 直送模型，归一化交给 AIPP（NPU / DvppImageProcessor 的产物）
struct DeviceTensor {
  const std::byte* ptr{nullptr};
  std::size_t bytes{0};
  int device_id{-1};
  Size size{};
  PixelFormat format{PixelFormat::kNv12};
};

using ModelInput = std::variant<HostTensor, DeviceTensor>;

}  // namespace avs_analyzer::io

#endif
