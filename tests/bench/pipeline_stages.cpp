#include "pipeline_stages.h"

#include <cstring>
#include <vector>

#include "imaging/frame.h"
#include "imaging/frame_storage.h"
#include "imaging/pixel_format.h"

namespace avsbench {

cv::Mat MakeYuvFrame(int width, int height, std::uint64_t seq) {
  // I420 布局（按字节偏移，避免行指针算错）：
  //   Y : [0, w*h)
  //   U : [w*h, w*h + (w/2)*(h/2))
  //   V : 紧随 U，同样 (w/2)*(h/2) 字节
  // 总字节 = w*h*3/2，正好等于 (h*3/2) 行 × w 列。
  const std::size_t y_size = static_cast<std::size_t>(width) * height;
  const std::size_t uv_size =
      static_cast<std::size_t>(width / 2) * (height / 2);

  std::vector<std::uint8_t> buffer(y_size + 2 * uv_size);

  // Y：斜向条纹随 seq 平移（画面是"动"的）
  for (int y = 0; y < height; ++y) {
    auto* row = buffer.data() + static_cast<std::ptrdiff_t>(y) * width;
    for (int x = 0; x < width; ++x) {
      row[x] = static_cast<std::uint8_t>((x + y + seq * 3) & 0xFF);
    }
  }
  // U / V：半分辨率
  auto* u = buffer.data() + y_size;
  auto* v = buffer.data() + y_size + uv_size;
  const int half_w = width / 2;
  const int half_h = height / 2;
  for (int y = 0; y < half_h; ++y) {
    auto* ur = u + static_cast<std::ptrdiff_t>(y) * half_w;
    auto* vr = v + static_cast<std::ptrdiff_t>(y) * half_w;
    for (int x = 0; x < half_w; ++x) {
      ur[x] = static_cast<std::uint8_t>((x * 2 + seq) & 0xFF);
      vr[x] = static_cast<std::uint8_t>((y * 2 + seq * 5) & 0xFF);
    }
  }

  cv::Mat yuv(height * 3 / 2, width, CV_8UC1, buffer.data());
  return yuv.clone();  // 让 Mat 拥有自己的数据
}

ConvertStage::ConvertStage(int in_width, int in_height, int out_width,
                           int out_height)
    : in_width_(in_width),
      in_height_(in_height),
      out_width_(out_width),
      out_height_(out_height) {}

cv::Mat ConvertStage::Run(const cv::Mat& yuv_i420) const {
  if (yuv_i420.empty()) return {};
  cv::Mat bgr;
  cv::cvtColor(yuv_i420, bgr, cv::COLOR_YUV2BGR_I420);
  if (out_width_ > 0 && out_height_ > 0 &&
      (bgr.cols != out_width_ || bgr.rows != out_height_)) {
    cv::resize(bgr, bgr, cv::Size(out_width_, out_height_));
  }
  return bgr;
}

PreprocessStage::PreprocessStage(const ModelSpec& spec)
    : processor_(), spec_(spec) {}

HostTensor PreprocessStage::Run(const cv::Mat& bgr) {
  if (bgr.empty()) return {};

  const cv::Mat src = bgr.isContinuous() ? bgr : bgr.clone();
  const std::size_t bytes =
      static_cast<std::size_t>(src.total()) * src.elemSize();
  std::vector<std::byte> buffer(bytes);
  if (bytes > 0) std::memcpy(buffer.data(), src.data, bytes);

  auto storage = std::make_shared<imaging::HostFrameStorage>(
      std::move(buffer), io::Size{src.cols, src.rows}, io::PixelFormat::kBgr24,
      /*pts=*/0);
  imaging::Frame frame(storage, io::MemoryLocation::kHost);

  io::ModelInput input = processor_.PrepareModelInput(frame, spec_);
  if (auto* tensor = std::get_if<HostTensor>(&input)) {
    return std::move(*tensor);
  }
  return {};
}

}  // namespace avsbench
