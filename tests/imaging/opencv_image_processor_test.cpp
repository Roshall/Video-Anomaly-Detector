// CPU 实现的精确行为测试（相对契约测试，这里断言数值细节）。
// 目的：把现有 inference.cpp 的 letterbox / NCHW / /255 行为原样钉住，
// 之后搬进 OpenCvImageProcessor 时不允许有任何行为漂移。
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "imaging/frame.h"
#include "imaging/frame_storage.h"
#include "imaging/opencv_image_processor.h"

#include "fake_frame_migrator.h"
#include "test_utils.h"

namespace avs_analyzer::imaging {
namespace {

// 用 OpenCV 生成真实 NV12（BT.601 full-range，正确 4:2:0 色度下采样），
// 模拟 VDEC 输出的像素格式。这个 OpenCV 版本没有 COLOR_BGR2YUV_NV12，
// 因此先出 I420（Y/U/V 三平面）再交错成 NV12 的 UV 平面——与
// OpenCvImageProcessor::ToBgr 里 COLOR_YUV2BGR_NV12 共用同一套色彩矩阵，
// 往返误差只在量化级别。
std::vector<std::byte> MakeNv12(const cv::Mat& bgr) {
  cv::Mat i420;
  cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
  const int w = bgr.cols, h = bgr.rows;
  const std::size_t y_sz = static_cast<std::size_t>(w) * h;
  std::vector<std::byte> out(y_sz * 3 / 2);
  auto* dst = reinterpret_cast<uchar*>(out.data());
  const auto* src = i420.ptr<uchar>();
  std::memcpy(dst, src, y_sz);  // Y 平面
  const auto* u = src + y_sz;
  const auto* v = u + y_sz / 4;
  auto* uv = dst + y_sz;
  const std::size_t uv_sz = y_sz / 4;
  for (std::size_t i = 0; i < uv_sz; ++i) {
    uv[2 * i + 0] = u[i];
    uv[2 * i + 1] = v[i];
  }
  return out;
}

// --- NCHW 布局与归一化 ---

TEST(OpenCvImageProcessorTest, NchwLayoutAndNormalizationAreExact) {
  OpenCvImageProcessor proc;
  auto migrator = std::make_shared<test::FakeFrameMigrator>();

  // 8x8 -> 8x8，无缩放无填充，便于逐值核对
  cv::Mat src(8, 8, CV_8UC3);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x)
      src.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uchar>(x + 1),
                                          static_cast<uchar>(y + 1),
                                          static_cast<uchar>(x * y + 3));

  Frame in(test::MakeHostFrame(src), MemoryLocation::kHost, migrator);
  ModelSpec spec;
  spec.input_size = Size{8, 8};

  ModelInput out = proc.PrepareModelInput(in, spec);
  ASSERT_TRUE(std::holds_alternative<HostTensor>(out));
  const auto& t = std::get<HostTensor>(out);

  ASSERT_EQ(t.dims[0], 1);
  ASSERT_EQ(t.dims[1], 3);
  ASSERT_EQ(t.dims[2], 8);
  ASSERT_EQ(t.dims[3], 8);
  ASSERT_EQ(t.data.size(), 1u * 3 * 8 * 8);

  const size_t hw = 8 * 8;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      const auto px = src.at<cv::Vec3b>(y, x);  // B,G,R
      const size_t i = static_cast<size_t>(y) * 8 + x;
      // 模型侧是 RGB：plane0=R=px[2], plane1=G=px[1], plane2=B=px[0]
      EXPECT_NEAR(t.data[i], px[2] / 255.0f, 1e-6f) << "R @" << x << "," << y;
      EXPECT_NEAR(t.data[hw + i], px[1] / 255.0f, 1e-6f) << "G @" << x << "," << y;
      EXPECT_NEAR(t.data[2 * hw + i], px[0] / 255.0f, 1e-6f) << "B @" << x << "," << y;
    }
  }
}

TEST(OpenCvImageProcessorTest, PaddingUses114DividedBy255) {
  OpenCvImageProcessor proc;
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  cv::Mat src(4, 8, CV_8UC3, cv::Scalar(10, 20, 30));  // 8x4 -> 8x8，上下各补 2

  Frame in(test::MakeHostFrame(src), MemoryLocation::kHost, migrator);
  ModelSpec spec;
  spec.input_size = Size{8, 8};

  ModelInput mi = proc.PrepareModelInput(in, spec);
  const auto& t = std::get<HostTensor>(mi);
  const size_t hw = 8 * 8;
  const float expect = 114.0f / 255.0f;

  for (int x = 0; x < 8; ++x) {
    for (int y : {0, 1, 6, 7}) {  // 填充行
      const size_t i = static_cast<size_t>(y) * 8 + x;
      EXPECT_NEAR(t.data[i], expect, 1e-6f);
      EXPECT_NEAR(t.data[hw + i], expect, 1e-6f);
      EXPECT_NEAR(t.data[2 * hw + i], expect, 1e-6f);
    }
  }
}

TEST(OpenCvImageProcessorTest, NormalizationCanBeDisabled) {
  OpenCvImageProcessor proc;
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  cv::Mat src(8, 8, CV_8UC3, cv::Scalar(0, 0, 255));  // 纯红（BGR）

  Frame in(test::MakeHostFrame(src), MemoryLocation::kHost, migrator);
  ModelSpec spec;
  spec.input_size = Size{8, 8};
  spec.normalize = false;

  ModelInput mi = proc.PrepareModelInput(in, spec);
  const auto& t = std::get<HostTensor>(mi);
  EXPECT_NEAR(t.data[0], 255.0f, 1e-6f) << "R 通道应为 255（未归一化）";
  EXPECT_NEAR(t.data[8 * 8], 0.0f, 1e-6f) << "G 通道";
  EXPECT_NEAR(t.data[2 * 8 * 8], 0.0f, 1e-6f) << "B 通道";
}

// --- 像素格式转换 ---

TEST(OpenCvImageProcessorTest, BgrToRgbIsExactChannelSwap) {
  OpenCvImageProcessor proc;
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  cv::Mat src(2, 3, CV_8UC3, cv::Scalar(1, 2, 3));  // B=1,G=2,R=3

  Frame in(test::MakeHostFrame(src), MemoryLocation::kHost, migrator);
  auto out = proc.ResizeAndConvert(in, Size{3, 2}, PixelFormat::kRgb24);

  ASSERT_NE(out, nullptr);
  const auto* p = reinterpret_cast<const uchar*>(out->pixels()->data());
  EXPECT_EQ(p[0], 3) << "RGB 首字节应是 R";
  EXPECT_EQ(p[1], 2);
  EXPECT_EQ(p[2], 1) << "RGB 末字节应是 B";
}

TEST(OpenCvImageProcessorTest, Nv12InputConvertsBackToPlausibleBgr) {
  OpenCvImageProcessor proc;
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  const cv::Mat src = test::MakePatternBgr(160, 90);
  auto nv12 = MakeNv12(src);

  auto storage = std::make_shared<HostFrameStorage>(std::move(nv12),
                                                    Size{160, 90}, PixelFormat::kNv12, 0);
  Frame in(storage, MemoryLocation::kHost, migrator);

  auto out = proc.ResizeAndConvert(in, Size{160, 90}, PixelFormat::kBgr24);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->size().w, 160);
  EXPECT_EQ(out->size().h, 90);
  EXPECT_EQ(out->format(), PixelFormat::kBgr24);

  cv::Mat got(90, 160, CV_8UC3,
              const_cast<void*>(static_cast<const void*>(out->pixels()->data())));
  const auto d = test::CompareBgr(got, src);
  EXPECT_LT(d.mean_diff, 12.0) << "NV12->BGR 往返不应出现系统性偏差";
}

// --- 元信息与错误处理 ---

TEST(OpenCvImageProcessorTest, ReportsCpuBackendAndAllOps) {
  OpenCvImageProcessor proc;
  EXPECT_EQ(proc.backend(), BackendKind::kCpu);
  EXPECT_TRUE(proc.Supports(Op::kResize));
  EXPECT_TRUE(proc.Supports(Op::kPixelFormatConvert));
  EXPECT_TRUE(proc.Supports(Op::kLetterbox));
  EXPECT_TRUE(proc.Supports(Op::kNormalize));
}

TEST(OpenCvImageProcessorTest, RejectsEmptyFrame) {
  OpenCvImageProcessor proc;
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  auto empty = std::make_shared<HostFrameStorage>(std::vector<std::byte>{},
                                                  Size{0, 0}, PixelFormat::kBgr24, 0);
  Frame in(empty, MemoryLocation::kHost, migrator);
  ModelSpec spec;
  spec.input_size = Size{640, 640};

  EXPECT_THROW(proc.PrepareModelInput(in, spec), std::invalid_argument);
  EXPECT_THROW(proc.ResizeAndConvert(in, Size{320, 180}, PixelFormat::kBgr24),
               std::invalid_argument);
}

}  // namespace
}  // namespace avs_analyzer::imaging
