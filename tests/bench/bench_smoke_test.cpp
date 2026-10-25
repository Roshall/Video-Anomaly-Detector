// ctest 冒烟用例：只做"不崩 + 不卡 + 无明显泄漏"的回归护栏。
//
// 阈值**故意放宽**——跑在 gtest 下、帧数只有几百，且 CI 机器噪声大。
// 真正的数值判定请用独立的 analyzer_pipeline_bench（--strict）。
//
// 这里与 bench_main 共用 RunBench()，保证 CI 与手工压测跑的是同一条路径。
#include <gtest/gtest.h>

#include "bench_harness.h"

namespace {

using namespace avsbench;

BenchOptions SmallOptions(MockMode mode) {
  BenchOptions o;
  o.frames = 300;
  o.warmup_frames = 50;
  o.phases = 1;
  o.width = 640;
  o.height = 360;
  o.fps = 25;
  o.realtime_pacing = false;
  o.mock.mode = mode;
  o.mock.cost_ns = 500'000;  // 0.5ms：够快，又能让 mock 自检有意义
  o.mock.cost_mode = CostMode::kSpin;
  o.mock.num_boxes = 2;
  o.inference_result_timeout_ms = 200;
  o.use_event_detector = true;
  o.use_admission = true;
  o.rss_every = 10;
  return o;
}

TEST(BenchSmoke, SyncModeShortRun) {
  const auto r = RunBench(SmallOptions(MockMode::kSync));

  ASSERT_TRUE(r.completed) << r.error;
  EXPECT_EQ(r.frames_total, 300u);
  EXPECT_GT(r.frames_submitted, 0u);
  // 没有一次"空等到超时"：说明 EventDetector 与送帧节奏是同步的。
  EXPECT_EQ(r.detect_wait_timeout, 0u);

  const auto growth = static_cast<std::int64_t>(r.rss_final_bytes) -
                      static_cast<std::int64_t>(r.rss_warmup_end_bytes);
  EXPECT_LT(growth, 64LL * 1024 * 1024) << "RSS 增长过大";
  EXPECT_LT(r.latency.e2e_ns.Summarize().p99_ns, 500'000'000LL);
}

TEST(BenchSmoke, QueuedModeShortRun) {
  auto o = SmallOptions(MockMode::kQueued);
  o.mock.mailbox_depth = 4;

  const auto r = RunBench(o);

  ASSERT_TRUE(r.completed) << r.error;
  EXPECT_GT(r.frames_submitted, 0u);
  EXPECT_GT(r.inferences, 0u);
  // 不允许凭空消失：真正跑过模型的 + 判为 stale 的，不超过提交量。
  EXPECT_LE(r.inferences + r.stale_dropped, r.frames_submitted);
  EXPECT_GT(r.mailbox_depth_peak, 0u) << "queued 模式应该观察到队列深度";
}

// 多路并发：验证 N 路流能同时跑起来，且投递量 = 每路帧数 × 流数。
TEST(BenchSmoke, MultiStreamShortRun) {
  auto o = SmallOptions(MockMode::kQueued);
  o.streams = 2;
  o.frames = 120;
  o.warmup_frames = 20;
  o.mock.cost_ns = 200'000;  // 0.2ms，避免冒烟用例太慢
  o.mock.mailbox_depth = 4;
  o.realtime_pacing = false;

  const auto r = RunBench(o);

  ASSERT_TRUE(r.completed) << r.error;
  EXPECT_EQ(r.streams, 2u);
  // --frames 是"每路"的帧数，总投递应为 2 倍。
  EXPECT_EQ(r.frames_offered, 240u);
  EXPECT_GT(r.frames_submitted, 0u);
  // 丢帧率分子永远不应超过分母（接纳数）。
  EXPECT_LE(r.stale_dropped + r.frames_lost, r.frames_submitted);
  EXPECT_GE(r.drop_rate, 0.0);
  EXPECT_LE(r.drop_rate, 1.0);
}

// 纯函数单测：分位数计算与时序无关，可以在任何环境下钉死。
TEST(BenchSmoke, LatencyStatsPercentiles) {
  LatencyStats st;
  st.Reserve(1000);
  for (std::int64_t i = 1; i <= 1000; ++i) st.Add(i);

  const auto s = st.Summarize();
  EXPECT_EQ(s.count, 1000u);
  EXPECT_EQ(s.min_ns, 1);
  EXPECT_EQ(s.max_ns, 1000);
  EXPECT_NEAR(s.mean_ns, 500.5, 0.01);
  EXPECT_NEAR(s.p50_ns, 500, 1);
  EXPECT_NEAR(s.p95_ns, 950, 2);
  EXPECT_NEAR(s.p99_ns, 990, 2);
}

}  // namespace
