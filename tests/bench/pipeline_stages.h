// 接入压测的两个管线阶段（按帧开销，用于定位瓶颈）。
//
// 为什么要接它们：mock 掉 YOLO 推理时，推理**内部**的 letterbox +
// HWC->NCHW + /255 也被一起跳过了，于是每帧最大的一块 CPU 开销从测量中
// 消失了。这里把它们接回来并各自单独计时，才能回答"瓶颈在哪"。
//
// 关于 YUV->BGR：生产代码用 FormatConvertor（内部 sws_scale），但该类以
// AVFrame 为输入、依赖 libav。当前环境 libavutil 链接不通（av_frame_alloc
// 无法解析），为了保持"可验证"，这里用 OpenCV 的 cvtColor 实现等价的
// YUV420 -> BGR 转换，开销量级一致；后续若接真实解码可一并换回生产实现。
#ifndef AVS_BENCH_PIPELINE_STAGES_H
#define AVS_BENCH_PIPELINE_STAGES_H

#include <cstdint>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "imaging/opencv_image_processor.h"
#include "io/tensor.h"

namespace avsbench {

// 中性契约层与成像层都在 avs_analyzer 命名空间下，这里做别名便于书写。
namespace io = avs_analyzer::io;
namespace imaging = avs_analyzer::imaging;

using io::HostTensor;
using io::ModelSpec;

/// 合成一帧 I420（YUV420p）数据：单个 CV_8UC1 的 Mat，高 = h * 3 / 2。
/// 按 seq 确定性变化（画面是"动"的），且不依赖外部视频文件。
cv::Mat MakeYuvFrame(int width, int height, std::uint64_t seq);

/// 阶段：YUV(I420) -> BGR。
class ConvertStage {
 public:
  ConvertStage(int in_width, int in_height, int out_width, int out_height);

  [[nodiscard]] cv::Mat Run(const cv::Mat& yuv_i420) const;

 private:
  int in_width_{0};
  int in_height_{0};
  int out_width_{0};
  int out_height_{0};
};

/// 阶段：BGR -> 模型输入张量。用**生产代码** OpenCvImageProcessor
/// （letterbox + HWC->NCHW + /255），语义与 inference.cpp 的预处理一致。
class PreprocessStage {
 public:
  explicit PreprocessStage(const ModelSpec& spec);

  [[nodiscard]] HostTensor Run(const cv::Mat& bgr);

 private:
  imaging::OpenCvImageProcessor processor_;
  ModelSpec spec_;
};

}  // namespace avsbench

#endif
