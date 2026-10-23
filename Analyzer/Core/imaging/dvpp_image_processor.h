// Strategy 的 NPU 实现：缩放 / 格式转换 / letterbox 由 VPC 在 NPU 上完成。
//
// 与 CPU 实现的语义差异（重要）：
//   - 归一化**不下沉到本模块**，而是交给推理节点的动态 AIPP，所以
//     Supports(Op::kNormalize) 返回 false；PrepareModelInput 直接产出
//     io::DeviceTensor（NV12 原样），不做 /255。
//   - 因此 DvppImageProcessor 必须与 AscendInferenceBackend 配对使用。
//
// 目前 VDEC/VPC 的真实调用待 CANN 环境接入；对 Host 帧暂时委托
// OpenCvImageProcessor 兜底，保证功能链路不断（等价于"自动回落"）。
#ifndef ANALYZER_IMAGING_DVPP_IMAGE_PROCESSOR_H
#define ANALYZER_IMAGING_DVPP_IMAGE_PROCESSOR_H

#include <memory>

#include "frame.h"
#include "frame_storage.h"
#include "image_processor.h"
#include "opencv_image_processor.h"
#include "pixel_format.h"

namespace avs_analyzer::imaging {

class DvppImageProcessor final : public IImageProcessor {
 public:
  explicit DvppImageProcessor(int device_id = 0);
  DvppImageProcessor(std::shared_ptr<const IFrameMigrator> migrator,
                     int device_id);

  [[nodiscard]] std::unique_ptr<Frame> ResizeAndConvert(
      const Frame& in, Size out, PixelFormat fmt) override;

  [[nodiscard]] ModelInput PrepareModelInput(const Frame& in,
                                             const ModelSpec& spec) override;

  [[nodiscard]] BackendKind backend() const noexcept override {
    return BackendKind::kNpu;
  }

  [[nodiscard]] bool Supports(Op op) const noexcept override;

 private:
  std::shared_ptr<const IFrameMigrator> migrator_;
  OpenCvImageProcessor cpu_fallback_;
  int device_id_{0};
};

}  // namespace avs_analyzer::imaging

#endif
