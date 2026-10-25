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
#include "io/tensor.h"
#include "monitor.h"
#include "task.h"

namespace avsbench {

// 中性契约层的命名空间在 avs_analyzer 下，这里做别名便于 bench 代码书写。
namespace io = avs_analyzer::io;
using io::ModelSpec;

/// kProbe 是 analyzer_timeout_test 里的手法：只实现最小 Analyzer，
/// 用来测"管线本身"；另两个走生产路径的 General/BunkOff。
enum class AnalyzerKind { kProbe, kGeneral, kBunkOff };

avs_analyzer::EventDetectorConfig DefaultEventConfig();

struct BenchOptions {
  // 帧
  /// **每路流**的帧数（总投递量 = frames * streams）。
  std::uint64_t frames{20000};
  std::uint64_t warmup_frames{2000};  // 不计入统计
  std::uint64_t phases{2};            // 稳态分段数，用于区分缓存增长与真泄漏

  /// 并发流数。>1 时每路流跑在自己的线程上（贴近真实：每个 Worker 一条线程，
  /// 共享同一个推理池），用于做容量测试。
  std::uint32_t streams{1};

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

  /// 接入真实组件（默认关：保持"纯管线"基线可复现）。
  /// use_convert    : 真实 FormatConvertor 做 YUV->BGR（每帧都做）
  /// use_preprocess : 真实 OpenCvImageProcessor 做 letterbox+NCHW+/255
  ///                  （只在帧被接纳时做，因为它属于推理的一部分）
  bool use_convert{false};
  bool use_preprocess{false};
  /// 预处理的目标输入尺寸（默认与 YOLO 一致：640x640）。
  ModelSpec preprocess_spec{};

  // 采样
  std::uint64_t rss_every{100};
  std::uint64_t max_wall_ms{0};  // 0 = 不启用看门狗
};

struct BenchReport {
  bool completed{false};
  std::string error{};

  std::uint32_t streams{1};
  /// 送进来的帧总数（= 每路帧数 × 流数）。
  std::uint64_t frames_offered{0};
  /// 同 frames_offered（保留旧字段名，冒烟用例在断言它）。
  std::uint64_t frames_total{0};
  /// 通过背压判定、系统确实想处理的帧数。
  std::uint64_t frames_submitted{0};
  std::uint64_t frames_skipped{0};
  std::uint64_t inferences{0};
  std::uint64_t stale_dropped{0};
  /// 被 mailbox "最新者胜" 覆盖掉、从未产出的帧数（静默丢失）。
  std::uint64_t frames_lost{0};
  std::uint64_t mailbox_depth_peak{0};

  /// **容量丢帧率** = (stale + 覆盖丢失) / 已接纳。
  /// 这是"系统扛不住"的度量：帧进来了、系统也想处理，但没算出结果。
  double drop_rate{0.0};
  /// 背压跳过率 = (投递 - 接纳) / 投递。
  /// 这是低采样（1~5fps）**设计使然**的跳过，**不是**容量问题，务必与
  /// drop_rate 分开看——混在一起会得到完全错误的结论。
  double admission_skip_rate{0.0};
  /// 目标帧率 = 流数 × fps；实际推理吞吐 = 推理次数 / 墙钟秒数。
  double target_fps{0.0};
  double effective_infer_fps{0.0};
  /// 需求帧率 = 接纳帧数 / 墙钟秒数，即"低采样之后系统真正想要的速率"。
  ///
  /// 它是判断容量的正确基准：拿入流 25~30fps 去比实际推理速率会得出
  /// "系统只能跑 15fps"的错误结论——那 15fps 其实是需求本身就这么低。
  double demand_fps{0.0};
  /// 理想墙钟（按时序应有的时长）与"跑慢了多少倍"。
  ///
  /// 这是**阻塞型生产者**下最诚实的容量信号：每路流提交后就阻塞等结果，
  /// 队列堆不起来，于是 mailbox 丢帧率会一直是 0，而真实代价体现在
  /// "同样的帧数花了更长的墙钟"——即 behind_factor > 1。
  double wall_expected_ms{0.0};
  double behind_factor{1.0};
  /// 观测到的最大滞后 = wanted_ts - frame_ts（毫秒）。
  /// 用来区分"低采样设计跳过"与"饱和导致的跳过"：滞后远大于一个 stride
  /// 就说明是扛不住了，而不是设计上不想要这一帧。
  std::int64_t max_lag_ms{0};
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
  /// 容量丢帧率上限（0 = 不校验）。注意别把 admission 跳过率算进来。
  double max_drop_rate{0.0};
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
