// CPU 解码器：实现 pipeline::IDecoder，内部走 FFmpeg（demux + 软解）。
//
// 头文件刻意不含任何 libav 类型：解码器是被 pipeline 工厂与业务层按
// IDecoder 使用的，不该把 FFmpeg 传染给调用方。所有 libav 细节关在 Impl 里，
// 因此本文件在没有 FFmpeg 头文件时也能被 include 和编译。
#ifndef ANALYZER_PIPELINE_FFMPEG_DECODER_H
#define ANALYZER_PIPELINE_FFMPEG_DECODER_H

#include <memory>

#include <opencv2/core.hpp>

#include "io/common.h"
#include "pipeline/pipeline_factory.h"

namespace avs_analyzer::pipeline {

using io::PixelFormat;

class FfmpegDecoder final : public IDecoder {
 public:
  FfmpegDecoder();
  ~FfmpegDecoder() override;

  FfmpegDecoder(const FfmpegDecoder&) = delete;
  FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

  [[nodiscard]] BackendKind backend() const noexcept override {
    return BackendKind::kCpu;
  }

  /// 打开文件或 RTSP：avformat_open_input → 找视频流 → 打开解码器。
  [[nodiscard]] bool Open(const StreamDesc& desc) override;

  void Close();

  [[nodiscard]] bool IsOpen() const noexcept;
  [[nodiscard]] int Width() const noexcept;
  [[nodiscard]] int Height() const noexcept;
  /// 码流的原始像素格式，已映射到中性契约的 io::PixelFormat。
  [[nodiscard]] PixelFormat pixel_format() const noexcept;

  /// 取下一帧并转成 BGR（Host）。返回 false 表示暂无帧或已到流尾。
  ///
  /// 这是 IDecoder 之外的附加能力：抽象层的 IDecoder 目前只定义 Open()，
  /// 帧泵送的统一接口留到 DetectionAlgorithm 接入时再补，避免现在就把
  /// 接口钉死成 FFmpeg 的形状。
  [[nodiscard]] bool ReceiveFrameTo(cv::Mat& bgr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace avs_analyzer::pipeline

#endif
