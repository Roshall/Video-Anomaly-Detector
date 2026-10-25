// 分阶段组件的测试（TDD）。
//
// 覆盖两个接入的阶段：
//   ConvertStage    YUV(I420) -> BGR
//   PreprocessStage BGR -> 模型张量，用**生产代码** OpenCvImageProcessor
//                   （letterbox + HWC->NCHW + /255）——这正是 mock 掉推理后
//                   被一并跳过的真实开销，必须补回来才能定位瓶颈
//
// 说明：YUV->BGR 这里用 OpenCV 的 cvtColor 实现，而没有直接用生产代码的
// FormatConvertor——后者以 AVFrame 为输入、依赖 libav。当前环境 libavutil
// 链接不通（av_frame_alloc 无法解析），为保持"可验证"才改用 OpenCV；
// 转换本身（YUV420 -> BGR）与生产路径等价，开销量级一致。
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "bench_harness.h"
#include "pipeline_stages.h"

namespace {

using namespace avsbench;

// ---------------------------------------------------------------- 合成 YUV 帧

TEST(PipelineStages, MakeYuvFrameHasI420Geometry) {
  const cv::Mat yuv = MakeYuvFrame(320, 240, 7);
  EXPECT_FALSE(yuv.empty());
  EXPECT_EQ(yuv.cols, 320);
  // I420：Y 平面 + U/V 各 1/4，总高 = h * 3 / 2
  EXPECT_EQ(yuv.rows, 240 * 3 / 2);
  EXPECT_EQ(yuv.type(), CV_8UC1);
}

TEST(PipelineStages, MakeYuvFrameIsDeterministicPerSequence) {
  const auto a = MakeYuvFrame(64, 48, 3);
  const auto b = MakeYuvFrame(64, 48, 3);
  const auto c = MakeYuvFrame(64, 48, 4);

  const auto luma = [](const cv::Mat& m) {
    return std::vector<unsigned char>(m.data, m.data + m.cols);
  };
  EXPECT_EQ(luma(a), luma(b)) << "同一 seq 必须产出同样的帧";
  EXPECT_NE(luma(a), luma(c)) << "不同 seq 应当不同（静止画面测不出真实负载）";
}

// ---------------------------------------------------------------- ConvertStage

TEST(PipelineStages, ConvertStageProducesBgrOfRequestedSize) {
  ConvertStage stage(320, 240, 320, 240);
  const cv::Mat yuv = MakeYuvFrame(320, 240, 11);

  const cv::Mat bgr = stage.Run(yuv);
  EXPECT_FALSE(bgr.empty());
  EXPECT_EQ(bgr.cols, 320);
  EXPECT_EQ(bgr.rows, 240);
  EXPECT_EQ(bgr.type(), CV_8UC3);
}

TEST(PipelineStages, ConvertStageOutputIsNotBlank) {
  ConvertStage stage(320, 240, 320, 240);
  const cv::Mat bgr = stage.Run(MakeYuvFrame(320, 240, 11));
  // 全零说明转换根本没生效
  EXPECT_GT(cv::mean(bgr)[0], 0.0);
  EXPECT_GT(cv::mean(bgr)[1], 0.0);
  EXPECT_GT(cv::mean(bgr)[2], 0.0);
}

TEST(PipelineStages, ConvertStageScalesToRequestedOutput) {
  ConvertStage stage(640, 480, 320, 240);
  const cv::Mat bgr = stage.Run(MakeYuvFrame(640, 480, 5));
  EXPECT_EQ(bgr.cols, 320);
  EXPECT_EQ(bgr.rows, 240);
}

// ---------------------------------------------------------------- PreprocessStage

TEST(PipelineStages, PreprocessStageProducesNormalizedNchwTensor) {
  io::ModelSpec spec;  // 默认 640x640, RGB, normalize=true, planar
  PreprocessStage stage(spec);

  const cv::Mat bgr(480, 640, CV_8UC3, cv::Scalar(10, 20, 30));
  const auto tensor = stage.Run(bgr);

  EXPECT_EQ(tensor.dims[0], 1);
  EXPECT_EQ(tensor.dims[1], 3);
  EXPECT_EQ(tensor.dims[2], spec.input_size.h);
  EXPECT_EQ(tensor.dims[3], spec.input_size.w);
  EXPECT_EQ(tensor.data.size(), static_cast<std::size_t>(1) * 3 *
                                    spec.input_size.h * spec.input_size.w);

  // normalize=true => 值必须落在 [0,1]，不能还是 0..255
  const auto [min_it, max_it] =
      std::minmax_element(tensor.data.begin(), tensor.data.end());
  EXPECT_GE(*min_it, 0.0f);
  EXPECT_LE(*max_it, 1.0f);
  EXPECT_GT(*max_it, 0.0f) << "全 0 说明归一化或排布出了问题";
}

TEST(PipelineStages, PreprocessStageIsDeterministic) {
  io::ModelSpec spec;
  PreprocessStage stage(spec);

  const cv::Mat bgr(360, 640, CV_8UC3, cv::Scalar(1, 2, 3));
  const auto t1 = stage.Run(bgr);
  const auto t2 = stage.Run(bgr);
  EXPECT_EQ(t1.data, t2.data);
}

// ---------------------------------------------------------------- 接入 harness 后的分阶段计时

BenchOptions SmallStagedOptions() {
  BenchOptions o;
  o.frames = 120;
  o.warmup_frames = 20;
  o.phases = 1;
  o.width = 640;
  o.height = 360;
  o.fps = 25;
  o.realtime_pacing = false;
  o.mock.mode = MockMode::kSync;
  o.mock.cost_ns = 200'000;
  o.mock.num_boxes = 2;
  o.inference_result_timeout_ms = 200;
  o.use_event_detector = true;
  o.use_admission = true;
  o.rss_every = 10;
  return o;
}

TEST(PipelineStages, ConvertStageTimedOnlyWhenEnabled) {
  {
    auto o = SmallStagedOptions();
    o.use_convert = true;
    const auto r = RunBench(o);
    ASSERT_TRUE(r.completed) << r.error;
    EXPECT_GT(r.latency.convert_ns.count(), 0u);
    EXPECT_GT(r.latency.convert_ns.Summarize().p50_ns, 0);
  }
  {
    auto o = SmallStagedOptions();
    o.use_convert = false;
    const auto r = RunBench(o);
    ASSERT_TRUE(r.completed) << r.error;
    EXPECT_EQ(r.latency.convert_ns.count(), 0u) << "未开启时不该有采样";
  }
}

TEST(PipelineStages, PreprocessStageTimedOnlyWhenEnabled) {
  {
    auto o = SmallStagedOptions();
    o.use_preprocess = true;
    const auto r = RunBench(o);
    ASSERT_TRUE(r.completed) << r.error;
    EXPECT_GT(r.latency.preprocess_ns.count(), 0u);
    EXPECT_GT(r.latency.preprocess_ns.Summarize().p50_ns, 0);
  }
  {
    auto o = SmallStagedOptions();
    o.use_preprocess = false;
    const auto r = RunBench(o);
    ASSERT_TRUE(r.completed) << r.error;
    EXPECT_EQ(r.latency.preprocess_ns.count(), 0u) << "未开启时不该有采样";
  }
}

// 每个阶段的耗时都不该超过端到端总耗时（防止计时埋点写错位置）。
TEST(PipelineStages, NoStageExceedsEndToEnd) {
  auto o = SmallStagedOptions();
  o.use_convert = true;
  o.use_preprocess = true;
  const auto r = RunBench(o);
  ASSERT_TRUE(r.completed) << r.error;

  const auto e2e = r.latency.e2e_ns.Summarize().p50_ns;
  EXPECT_LE(r.latency.convert_ns.Summarize().p50_ns, e2e);
  EXPECT_LE(r.latency.preprocess_ns.Summarize().p50_ns, e2e);
  EXPECT_LE(r.latency.submit_ns.Summarize().p50_ns, e2e);
  EXPECT_LE(r.latency.wait_ns.Summarize().p50_ns, e2e);
}

}  // namespace
