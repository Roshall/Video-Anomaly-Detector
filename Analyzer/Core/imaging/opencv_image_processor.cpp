#include "opencv_image_processor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace avs_analyzer::imaging {
namespace {

constexpr int kLetterboxPadValue = 114;  // 与迁移前 inference.cpp 保持一致

void ThrowIfInvalid(const IFrameStorage& s) {
  if (s.location() != MemoryLocation::kHost) {
    throw std::invalid_argument(
        "OpenCvImageProcessor: input must be materialized on host first");
  }
  if (s.size().w <= 0 || s.size().h <= 0 || s.data() == nullptr) {
    throw std::invalid_argument("OpenCvImageProcessor: empty frame");
  }
  if (s.byte_size() < FrameBytes(s.size(), s.format())) {
    throw std::invalid_argument("OpenCvImageProcessor: truncated frame buffer");
  }
}

// I420（Y、U、V 三平面）-> NV12（Y 平面 + 交错 UV）
std::vector<std::byte> I420ToNv12(const uchar* i420, int w, int h, bool swap_uv) {
  const auto y_bytes = static_cast<std::size_t>(w) * h;
  std::vector<std::byte> out(y_bytes * 3 / 2);
  auto* dst = reinterpret_cast<uchar*>(out.data());
  std::copy_n(i420, y_bytes, dst);

  const uchar* u = i420 + y_bytes;
  const uchar* v = u + (y_bytes / 4);
  const uchar* first = swap_uv ? v : u;  // NV21 先放 V
  const uchar* second = swap_uv ? u : v;

  auto* uv = dst + y_bytes;
  for (std::size_t i = 0; i < y_bytes / 4; ++i) {
    uv[2 * i + 0] = first[i];
    uv[2 * i + 1] = second[i];
  }
  return out;
}

std::vector<std::byte> BgrToTarget(const cv::Mat& bgr, PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::kBgr24: {
      std::vector<std::byte> out(static_cast<std::size_t>(bgr.total()) *
                                 bgr.elemSize());
      std::memcpy(out.data(), bgr.data, out.size());
      return out;
    }
    case PixelFormat::kRgb24: {
      cv::Mat rgb;
      cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
      std::vector<std::byte> out(static_cast<std::size_t>(rgb.total()) *
                                 rgb.elemSize());
      std::memcpy(out.data(), rgb.data, out.size());
      return out;
    }
    case PixelFormat::kNv12:
    case PixelFormat::kNv21:
    case PixelFormat::kYuv420p: {
      cv::Mat i420;
      cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
      if (fmt == PixelFormat::kYuv420p) {
        std::vector<std::byte> out(static_cast<std::size_t>(i420.total()) *
                                   i420.elemSize());
        std::memcpy(out.data(), i420.data, out.size());
        return out;
      }
      return I420ToNv12(i420.data, bgr.cols, bgr.rows,
                        /*swap_uv=*/fmt == PixelFormat::kNv21);
    }
    default:
      throw std::invalid_argument("OpenCvImageProcessor: unsupported target format");
  }
}

}  // namespace

cv::Mat OpenCvImageProcessor::ToBgr(const Frame& in) {
  const auto s = in.pixels();
  ThrowIfInvalid(*s);

  const int w = s->size().w;
  const int h = s->size().h;
  const auto* p = reinterpret_cast<const uchar*>(s->data());

  switch (s->format()) {
    case PixelFormat::kBgr24:
      return cv::Mat(h, w, CV_8UC3, const_cast<uchar*>(p));
    case PixelFormat::kRgb24: {
      cv::Mat rgb(h, w, CV_8UC3, const_cast<uchar*>(p));
      cv::Mat bgr;
      cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
      return bgr;
    }
    case PixelFormat::kNv12: {
      cv::Mat yuv(h * 3 / 2, w, CV_8UC1, const_cast<uchar*>(p));
      cv::Mat bgr;
      cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_NV12);
      return bgr;
    }
    case PixelFormat::kNv21: {
      cv::Mat yuv(h * 3 / 2, w, CV_8UC1, const_cast<uchar*>(p));
      cv::Mat bgr;
      cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_NV21);
      return bgr;
    }
    case PixelFormat::kYuv420p: {
      cv::Mat yuv(h * 3 / 2, w, CV_8UC1, const_cast<uchar*>(p));
      cv::Mat bgr;
      cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
      return bgr;
    }
    default:
      throw std::invalid_argument("OpenCvImageProcessor: unsupported source format");
  }
}

io::LetterboxInfo OpenCvImageProcessor::LetterboxGeometry(const cv::Mat& bgr,
                                                            Size dst) {
  const float r = std::min(static_cast<float>(dst.w) / bgr.cols,
                           static_cast<float>(dst.h) / bgr.rows);
  const int new_w = static_cast<int>(std::round(bgr.cols * r));
  const int new_h = static_cast<int>(std::round(bgr.rows * r));
  return {r, (dst.w - new_w) / 2, (dst.h - new_h) / 2};
}

cv::Mat OpenCvImageProcessor::Letterbox(const cv::Mat& bgr, Size dst, int pad) {
  if (bgr.empty() || dst.w <= 0 || dst.h <= 0) {
    throw std::invalid_argument("OpenCvImageProcessor::Letterbox: invalid input");
  }
  const auto geom = LetterboxGeometry(bgr, dst);
  const int new_w = static_cast<int>(std::round(bgr.cols * geom.scale));
  const int new_h = static_cast<int>(std::round(bgr.rows * geom.scale));
  const int pad_x = geom.pad_x;
  const int pad_y = geom.pad_y;

  cv::Mat resized;
  cv::resize(bgr, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
  cv::Mat canvas(dst.h, dst.w, CV_8UC3, cv::Scalar(pad, pad, pad));
  resized.copyTo(canvas(cv::Rect(pad_x, pad_y, new_w, new_h)));
  return canvas;
}

std::unique_ptr<Frame> OpenCvImageProcessor::ResizeAndConvert(const Frame& in,
                                                              Size out,
                                                              PixelFormat fmt) {
  const cv::Mat bgr = ToBgr(in);  // 非法输入在此抛出
  if (out.w <= 0 || out.h <= 0) {
    throw std::invalid_argument("OpenCvImageProcessor: invalid target size");
  }

  cv::Mat resized;
  cv::resize(bgr, resized, cv::Size(out.w, out.h), 0, 0, cv::INTER_LINEAR);

  auto bytes = BgrToTarget(resized, fmt);
  auto storage = std::make_shared<HostFrameStorage>(std::move(bytes), out, fmt,
                                                    in.pts());
  return std::make_unique<Frame>(storage, MemoryLocation::kHost, migrator_);
}

ModelInput OpenCvImageProcessor::PrepareModelInput(const Frame& in,
                                                   const ModelSpec& spec) {
  const cv::Mat bgr = ToBgr(in);  // 非法输入在此抛出
  const cv::Mat canvas = Letterbox(bgr, spec.input_size, kLetterboxPadValue);

  cv::Mat rgb;
  cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);

  const int h = rgb.rows;
  const int w = rgb.cols;
  const auto hw = static_cast<std::size_t>(h) * w;

  HostTensor t;
  t.dims = {1, 3, h, w};
  t.letterbox = LetterboxGeometry(bgr, spec.input_size);
  t.data.resize(3 * hw);

  constexpr float kInv255 = 1.0f / 255.0f;
  float* c0 = t.data.data();
  float* c1 = c0 + hw;
  float* c2 = c1 + hw;

  for (int y = 0; y < h; ++y) {
    const auto* row = rgb.ptr<cv::Vec3b>(y);
    for (int x = 0; x < w; ++x) {
      const auto px = row[x];  // R,G,B
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      const float s = spec.normalize ? kInv255 : 1.0f;
      c0[i] = px[0] * s;
      c1[i] = px[1] * s;
      c2[i] = px[2] * s;
    }
  }
  return t;
}

}  // namespace avs_analyzer::imaging
