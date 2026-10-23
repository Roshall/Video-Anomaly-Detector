// 契约测试（关键）：同一套断言跑在**所有** IImageProcessor 实现上。
// 现在只注册 CPU 实现；将来 DvppImageProcessor（NPU）加进来时只要在这里
// 多注册一行，就能证明两条路径像素级等价 —— 这是"两套图像处理可互换"的底线。
//
// 注册新实现：
//   ProcessorEntry{"dvpp", [] { return std::make_unique<DvppImageProcessor>(...); }},
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>

#include "imaging/image_processor.h"
#include "imaging/opencv_image_processor.h"

#include "fake_frame_migrator.h"
#include "test_utils.h"

namespace avs_analyzer::imaging {
namespace {

struct ProcessorEntry {
  std::string name;
  std::function<std::unique_ptr<IImageProcessor>()> create;
};

class ImageProcessorContract : public ::testing::TestWithParam<ProcessorEntry> {
 protected:
  std::unique_ptr<IImageProcessor> Make() const { return GetParam().create(); }
};

INSTANTIATE_TEST_SUITE_P(
    ImagingBackends, ImageProcessorContract,
    ::testing::Values(
        ProcessorEntry{"opencv",
                       [] { return std::make_unique<OpenCvImageProcessor>(); }}
        // , ProcessorEntry{"dvpp", [] { return std::make_unique<DvppImageProcessor>(...); }}
        //   ^ Phase 6 接入 NPU 实现后取消注释；硬件不可用时改由
        //     GTEST_SKIP() 的工厂返回 nullptr 并在 SetUp 里跳过
        ),
    [](const ::testing::TestParamInfo<ProcessorEntry>& info) {
      return info.param.name;
    });

// --- 几何与填充：逐像素精确 ---

TEST_P(ImageProcessorContract, LetterboxKeepsAspectRatioAndPadsWith114) {
  auto proc = Make();
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  const cv::Mat src = test::MakePatternBgr(320, 240);  // 4:3
  Frame in(test::MakeHostFrame(src), MemoryLocation::kHost, migrator);

  ModelSpec spec;
  spec.input_size = Size{640, 640};
  ModelInput out = proc->PrepareModelInput(in, spec);

  cv::Mat got = test::ModelInputToBgr(out, *migrator);
  ASSERT_EQ(got.cols, 640);
  ASSERT_EQ(got.rows, 640);

  // 参考几何：scale=2.0 -> 640x480，上下各补 80，左右不补
  const auto ref = test::ExpectedLetterbox(Size{320, 240}, Size{640, 640});
  ASSERT_EQ(ref.new_w, 640);
  ASSERT_EQ(ref.new_h, 480);
  ASSERT_EQ(ref.pad_x, 0);
  ASSERT_EQ(ref.pad_y, 80);

  // 填充带必须严格是 114（BGR 三通道都等于 114）
  for (int x = 0; x < 640; ++x) {
    for (int y : {0, 40, 79, 560, 600, 639}) {
      const auto px = got.at<cv::Vec3b>(y, x);
      EXPECT_EQ(px[0], 114) << "x=" << x << " y=" << y;
      EXPECT_EQ(px[1], 114) << "x=" << x << " y=" << y;
      EXPECT_EQ(px[2], 114) << "x=" << x << " y=" << y;
    }
  }

  // 内容带不能是常量填充
  const auto mid = got.at<cv::Vec3b>(240, 320);
  EXPECT_FALSE(mid[0] == 114 && mid[1] == 114 && mid[2] == 114);
}

TEST_P(ImageProcessorContract, LetterboxMatchesIndependentReference) {
  auto proc = Make();
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  const cv::Mat src = test::MakePatternBgr(640, 360);  // 16:9
  Frame in(test::MakeHostFrame(src), MemoryLocation::kHost, migrator);

  ModelSpec spec;
  spec.input_size = Size{640, 640};
  ModelInput out = proc->PrepareModelInput(in, spec);

  cv::Mat got = test::ModelInputToBgr(out, *migrator);
  cv::Mat expected = test::ExpectedLetterboxBgr(src, Size{640, 640});

  const auto d = test::CompareBgr(got, expected);
  EXPECT_LE(d.max_diff, 2) << "与独立参考实现的最大通道差";
  EXPECT_LE(d.mean_diff, 0.5);
}

TEST_P(ImageProcessorContract, NonSquareInputIsNotStretched) {
  auto proc = Make();
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  // 故意用"内容不含 114"的图，这样"整行等于 114"只可能是填充带
  cv::Mat src(200, 400, CV_8UC3, cv::Scalar(10, 20, 30));
  Frame in(test::MakeHostFrame(src), MemoryLocation::kHost, migrator);

  ModelSpec spec;
  spec.input_size = Size{400, 400};
  ModelInput out = proc->PrepareModelInput(in, spec);

  cv::Mat got = test::ModelInputToBgr(out, *migrator);
  ASSERT_EQ(got.rows, 400);

  // 400x200 -> 400x200，上下各补 100
  int pad_rows = 0;
  for (int y = 0; y < 400; ++y) {
    bool all_pad = true;
    for (int x = 0; x < 400; ++x) {
      if (got.at<cv::Vec3b>(y, x) != cv::Vec3b(114, 114, 114)) { all_pad = false; break; }
    }
    if (all_pad) ++pad_rows;
  }
  EXPECT_EQ(pad_rows, 200) << "上下各 100 行填充，共 200 行";
}

// --- 展示/推流用的缩放与格式转换 ---

TEST_P(ImageProcessorContract, ResizeAndConvertHonoursRequestedSizeAndFormat) {
  auto proc = Make();
  auto migrator = std::make_shared<test::FakeFrameMigrator>();
  Frame in(test::MakeHostFrame(test::MakePatternBgr(1920, 1080)),
           MemoryLocation::kHost, migrator);

  auto out = proc->ResizeAndConvert(in, Size{320, 180}, PixelFormat::kBgr24);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->size().w, 320);
  EXPECT_EQ(out->size().h, 180);
  EXPECT_EQ(out->format(), PixelFormat::kBgr24);
  EXPECT_EQ(out->pixels()->location(), MemoryLocation::kHost);

  auto rgb = proc->ResizeAndConvert(in, Size{320, 180}, PixelFormat::kRgb24);
  ASSERT_NE(rgb, nullptr);
  EXPECT_EQ(rgb->format(), PixelFormat::kRgb24);
}

// --- 元信息契约 ---

TEST_P(ImageProcessorContract, BackendIsStableAcrossCalls) {
  auto proc = Make();
  const auto b1 = proc->backend();
  const auto b2 = proc->backend();
  EXPECT_EQ(b1, b2);
}

TEST_P(ImageProcessorContract, SupportsCoreOps) {
  auto proc = Make();
  EXPECT_TRUE(proc->Supports(Op::kResize));
  EXPECT_TRUE(proc->Supports(Op::kPixelFormatConvert));
  EXPECT_TRUE(proc->Supports(Op::kLetterbox));
}

}  // namespace
}  // namespace avs_analyzer::imaging
