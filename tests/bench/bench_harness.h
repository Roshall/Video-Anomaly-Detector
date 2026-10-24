// 压测装配：把 Config / Monitor / Analyzer / MockInference / EventDetector
// 组装起来跑帧循环，产出内存与延迟报告。
//
// 这份代码被 bench_main.cpp（独立可执行）与 bench_smoke_test.cpp（ctest 冒烟）
// **共用**，保证手工压测与 CI 跑的是同一条路径。
#ifndef AVS_BENCH_BENCH_HARNESS_H
#define AVS_BENCH_BENCH_HARNESS_H

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include "latency_stats.h"
#include "mock_inference.h"
#include "rss_sampler.h"

#include "admission.h"
#include "analyzer.h"
#include "event.h"
#include "monitor.h"
#include "task.h"

namespace avsbench {

/// kProbe 是 analyzer_timeout_test 里的手法：只实现最小 Analyzer，
/// 用来测"管线本身"；另两个走生产路径的 General/BunkOff。
enum class AnalyzerKind { kProbe, kGeneral, kBunkOff };

avs_analyzer::EventDetectorConfig DefaultEventConfig();

struct BenchOptions {
  // 帧
  std::uint64_t frames{20000};
  std::uint64_t warmup_frames{2000};  // 不计入统计
  std::uint64_t phases{2};            // 稳态分段数，用于区分缓存增长与真泄漏

  // 合成帧
  int width{1920};
  int height{1080};
  bool reuse_image{true};  // false = 每帧 clone（把图像生命周期纳入测量）

  // 节奏
  double fps{25.0};
  bool realtime_pacing{false};  // true = 按墙钟对齐 fps

  // mock 推理
  MockInferenceConfig mock;

  // Analyzer
  AnalyzerKind analyzer_kind{AnalyzerKind::kProbe};
  avs_analyzer::Task task{avs_analyzer::Task::kBreakIn};
  /// 必须调小：默认 5000ms，一旦 EventDetector 与 mock 的 ts 推进不同步，
  /// 每次都会空等满 5 秒，压测直接失去意义。
  int inference_result_timeout_ms{300};

  // 事件状态机
  bool use_event_detector{true};
  avs_analyzer::EventDetectorConfig event_cfg{DefaultEventConfig()};

  // 背压
  bool use_admission{true};

  // 采样
  std::uint64_t rss_every{100};
  std::uint64_t max_wall_ms{0};  // 0 = 不启用看门狗
};

struct BenchReport {
  bool completed{false};
  std::string error{};

  std::uint64_t frames_total{0};
  std::uint64_t frames_submitted{0};
  std::uint64_t frames_skipped{0};
  std::uint64_t inferences{0};
  std::uint64_t stale_dropped{0};
  std::uint64_t mailbox_depth_peak{0};
  /// >0 说明发生过"空等到超时"，即 EventDetector 与送帧节奏不同步。
  std::uint64_t detect_wait_timeout{0};
  std::uint64_t event_starts{0};

  LatencyBuckets latency;

  std::uint64_t rss_warmup_end_bytes{0};
  std::uint64_t rss_phase1_end_bytes{0};
  std::uint64_t rss_final_bytes{0};
  std::uint64_t rss_peak_bytes{0};
  double slope_bytes_per_frame{0.0};
  std::int64_t phase1_growth{0};
  std::int64_t phase2_growth{0};

  double wall_ms{0.0};
  double achieved_fps{0.0};

  [[nodiscard]] nlohmann::json ToJson() const;
  [[nodiscard]] std::string ToText() const;
};

struct BenchThresholds {
  std::uint64_t max_phase2_growth_bytes{2u * 1024 * 1024};  // 2 MiB，最硬判据
  double max_slope_bytes_per_frame{200.0};
  std::uint64_t max_final_over_warmup_bytes{8u * 1024 * 1024};
  std::uint64_t max_peak_over_final_bytes{32u * 1024 * 1024};
  std::int64_t max_p99_e2e_ns{0};          // 0 = 不校验
  std::uint64_t max_detect_wait_timeout{0};
};

struct BenchVerdict {
  bool pass{false};
  std::vector<std::string> failures;
};

BenchReport RunBench(const BenchOptions& options);
BenchVerdict Evaluate(const BenchReport& report,
                      const BenchThresholds& thresholds);

}  // namespace avsbench

#endif
