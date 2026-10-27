// Strategy 的 CPU 实现：把现有 inference.cpp 的预处理行为原样搬进来。
//   letterbox（等比缩放 + 114 灰填充）→ BGR→RGB → HWC→NCHW → /255
// 行为必须与迁移前逐像素一致，由 opencv_image_processor_test.cpp 钉死。
#ifndef ANALYZER_IMAGING_OPENCV_IMAGE_PROCESSOR_H
#define ANALYZER_IMAGING_OPENCV_IMAGE_PROCESSOR_H

#include <memory>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "frame.h"
#include "frame_storage.h"
#include "image_processor.h"
#include "pixel_format.h"

namespace avs_analyzer::imaging {

class OpenCvImageProcessor final : public IImageProcessor {
 public:
  OpenCvImageProcessor() = default;
  explicit OpenCvImageProcessor(std::shared_ptr<const IFrameMigrator> migrator)
      : migrator_(std::move(migrator)) {}

  [[nodiscard]] std::unique_ptr<Frame> ResizeAndConvert(
      const Frame& in, Size out, PixelFormat fmt) override;

  [[nodiscard]] ModelInput PrepareModelInput(const Frame& in,
                                             const ModelSpec& spec) override;

  [[nodiscard]] BackendKind backend() const noexcept override {
    return BackendKind::kCpu;
  }
  [[nodiscard]] bool Supports(Op) const noexcept override { return true; }

  /// 把任意受支持格式的帧读成 CV_8UC3 BGR（Host）。供测试与复用。
  [[nodiscard]] static cv::Mat ToBgr(const Frame& in);

  /// 等比缩放到 dst 并用 pad 值居中填充（与 inference.cpp 语义一致）
  [[nodiscard]] static cv::Mat Letterbox(const cv::Mat& bgr, Size dst,
                                         int pad = 114);

  /// letterbox 的几何参数。与 Letterbox() 共用同一套计算，保证张量里携带的
  /// scale/pad 与画布一致——否则坐标还原会错。
  [[nodiscard]] static io::LetterboxInfo LetterboxGeometry(const cv::Mat& bgr,
                                                            Size dst);

 private:
  std::shared_ptr<const IFrameMigrator> migrator_;
};

}  // namespace avs_analyzer::imaging

#endif
