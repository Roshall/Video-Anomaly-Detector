// 图像处理测试的公共工具：构造帧、独立实现的参考 letterbox、ModelInput 取回 Host。
//
// 注意：这里刻意用"独立实现"而非复用被测代码来算期望值，否则测试变成同义反复。
// 重采样核（cv::INTER_LINEAR）无法独立复现，因此约定：
//   - 几何（缩放比例 / padding 偏移）与填充值 —— 逐像素精确断言
//   - 重采样结果本身 —— 只在契约测试里做容差比对
#ifndef ANALYZER_TESTS_IMAGING_TEST_UTILS_H
#define ANALYZER_TESTS_IMAGING_TEST_UTILS_H

#include <cmath>
#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "imaging/frame.h"
#include "imaging/frame_storage.h"
#include "imaging/image_processor.h"

namespace avs_analyzer::imaging::test {

// ---- 构造 ----

// 生成一张确定性的 BGR 测试图：像素值随坐标变化，便于定位缩放/布局错误。
inline cv::Mat MakePatternBgr(int w, int h) {
  cv::Mat img(h, w, CV_8UC3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      img.at<cv::Vec3b>(y, x) = cv::Vec3b(
          static_cast<uchar>((x * 7 + y * 3) & 0xFF),
          static_cast<uchar>((x * 3 + y * 11) & 0xFF),
          static_cast<uchar>((x * 13 + y * 5) & 0xFF));
    }
  }
  return img;
}

inline std::shared_ptr<HostFrameStorage> MakeHostFrame(const cv::Mat& bgr,
                                                       int64_t pts = 0) {
  // 目前只支持 CV_8UC3，调用方保证
  std::vector<std::byte> buf(static_cast<size_t>(bgr.total() * bgr.elemSize()));
  std::memcpy(buf.data(), bgr.data, buf.size());
  return std::make_shared<HostFrameStorage>(
      std::move(buf), Size{bgr.cols, bgr.rows}, PixelFormat::kBgr24, pts);
}

// ---- 独立实现的参考 letterbox（不复用被测代码）----

struct LetterboxRef {
  double scale = 1.0;
  int pad_x = 0;
  int pad_y = 0;
  int new_w = 0;
  int new_h = 0;
};

inline LetterboxRef ExpectedLetterbox(Size src, Size dst) {
  LetterboxRef r;
  r.scale = std::min(static_cast<double>(dst.w) / src.w,
                     static_cast<double>(dst.h) / src.h);
  r.new_w = static_cast<int>(std::round(src.w * r.scale));
  r.new_h = static_cast<int>(std::round(src.h * r.scale));
  r.pad_x = (dst.w - r.new_w) / 2;
  r.pad_y = (dst.h - r.new_h) / 2;
  return r;
}

// 参考结果图：dst 尺寸，填充常量 114（与现有 inference.cpp 的 cv::Scalar(114,114,114) 一致）
inline cv::Mat ExpectedLetterboxBgr(const cv::Mat& src, Size dst, uchar pad = 114) {
  const auto r = ExpectedLetterbox(Size{src.cols, src.rows}, dst);
  cv::Mat canvas(dst.h, dst.w, CV_8UC3, cv::Scalar(pad, pad, pad));
  cv::Mat resized;
  cv::resize(src, resized, cv::Size(r.new_w, r.new_h), 0, 0, cv::INTER_LINEAR);
  resized.copyTo(canvas(cv::Rect(r.pad_x, r.pad_y, r.new_w, r.new_h)));
  return canvas;
}

// ---- 从 ModelInput 取回可比较的 Host 图像 ----

// HostTensor(NCHW float) -> CV_8UC3 BGR
inline cv::Mat HostTensorToBgr(const HostTensor& t) {
  const int n = static_cast<int>(t.dims[0]);
  const int c = static_cast<int>(t.dims[1]);
  const int h = static_cast<int>(t.dims[2]);
  const int w = static_cast<int>(t.dims[3]);
  if (n != 1 || c != 3) return {};
  cv::Mat bgr(h, w, CV_8UC3);
  const size_t hw = static_cast<size_t>(h) * w;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      // NCHW: 平面 0/1/2 分别是 R/G/B（模型侧顺序），转回 BGR
      const float r = t.data[i] * 255.0f;
      const float g = t.data[hw + i] * 255.0f;
      const float b = t.data[2 * hw + i] * 255.0f;
      bgr.at<cv::Vec3b>(y, x) = cv::Vec3b(
          static_cast<uchar>(std::clamp(b, 0.0f, 255.0f)),
          static_cast<uchar>(std::clamp(g, 0.0f, 255.0f)),
          static_cast<uchar>(std::clamp(r, 0.0f, 255.0f)));
    }
  }
  return bgr;
}

// 任意 ModelInput -> CV_8UC3 BGR（DeviceTensor 经 migrator 回 Host 再转）
inline cv::Mat ModelInputToBgr(const ModelInput& in, const IFrameMigrator& migrator) {
  if (std::holds_alternative<HostTensor>(in)) return HostTensorToBgr(std::get<HostTensor>(in));

  const auto& d = std::get<DeviceTensor>(in);
  auto host = migrator.ToHost(
      DeviceFrameStorage(d.ptr, d.bytes, d.size, d.format, /*pts=*/0, d.device_id));
  cv::Mat src;
  if (d.format == PixelFormat::kNv12) {
    cv::Mat yuv(d.size.h * 3 / 2, d.size.w, CV_8UC1,
                const_cast<void*>(static_cast<const void*>(host->data())));
    cv::cvtColor(yuv, src, cv::COLOR_YUV2BGR_NV12);
  } else {
    cv::Mat(d.size.h, d.size.w, CV_8UC3,
            const_cast<void*>(static_cast<const void*>(host->data())))
        .copyTo(src);
  }
  return src;
}

// 两图最大/平均通道差
struct DiffStat {
  int max_diff = 0;
  double mean_diff = 0.0;
};

inline DiffStat CompareBgr(const cv::Mat& a, const cv::Mat& b) {
  if (a.size() != b.size() || a.type() != b.type()) return {255, 255.0};
  DiffStat s;
  long long total = 0;
  const int count = a.rows * a.cols * a.channels();
  for (int y = 0; y < a.rows; ++y) {
    for (int x = 0; x < a.cols; ++x) {
      const auto pa = a.at<cv::Vec3b>(y, x);
      const auto pb = b.at<cv::Vec3b>(y, x);
      for (int c = 0; c < 3; ++c) {
        const int d = std::abs(static_cast<int>(pa[c]) - static_cast<int>(pb[c]));
        s.max_diff = std::max(s.max_diff, d);
        total += d;
      }
    }
  }
  s.mean_diff = count ? static_cast<double>(total) / count : 0.0;
  return s;
}

}  // namespace avs_analyzer::imaging::test

#endif
