// 模拟的 YOLO 推理：替换真实 ONNX Runtime 计算，其余链路保持真实。
//
// 注入方式：Analyzer::SetAddTaskCallback() 接收本类 Register() 返回的回调，
// **不修改任何生产代码**。
//
// 两种模式：
//   - kSync  ：AddTask 内同步算完并回调，测纯管线延迟下限
//   - kQueued：走真实的 AnalyzerMailboxes + 取任务线程，测队列内存与背压
//
// 行为刻意逐行对齐 Analyzer/Core/detector/yolo_detector_onnx.cpp 的
// YOLODetector::Run()——尤其是 stale 语义：**stale 时结果置空，但仍必须调用
// 回调**。不回调会让 FetchRawDetection 空等到超时，延迟/队列/丢帧全部失真。
#ifndef AVS_BENCH_MOCK_INFERENCE_H
#define AVS_BENCH_MOCK_INFERENCE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <opencv2/core.hpp>

#include "detector/constant_yolo.h"  // DetectorPool::AddTaskFunc / Callback / ImagePtr
#include "detector/inference_types.h"  // DL_RESULT
#include "detector/mailbox.h"          // AnalyzerMailboxes / SharedInference
#include "metrics.h"                   // ModelMetrics

namespace avsbench {

enum class MockMode { kSync, kQueued };

/// 模拟耗时的注入方式。
enum class CostMode {
  kSpin,    // 忙等：最精确（误差 < 20us），但占满一个核
  kSleep,   // 让出 CPU：贴近真实加速器推理时的调度行为，但超调 50~150us
  kHybrid,  // sleep 到剩 200us 再忙等：折中
};

struct MockInferenceConfig {
  MockMode mode{MockMode::kSync};
  CostMode cost_mode{CostMode::kSpin};

  std::uint64_t cost_ns{2'000'000};  // 固定模拟耗时（默认 2ms）
  std::uint64_t jitter_ns{0};        // 叠加 [0, jitter_ns) 均匀抖动
  std::uint64_t seed{12345};

  std::size_t mailbox_depth{1};   // 1 = live；4 = offline backlog
  unsigned worker_threads{1};     // queued 模式的取任务线程数

  int num_boxes{2};
  int class_id{0};
  float confidence{0.90f};
  /// true = 框随帧号确定性平移（让 ByteTrack 有匹配/新建/丢失）；
  /// false = 框完全静止（ByteTrack 恒定 1 条 track，是查内存泄漏的干净对照组）。
  bool move_boxes{true};
  cv::Size frame_size{1920, 1080};

  /// 复刻真实 stale 语义（置空但仍回调）。**不要关掉**，关掉会让测量失真，
  /// 这个开关只是用来演示"不回调"会造成什么后果。
  bool replicate_stale{true};
};

class MockInference {
 public:
  using Callback = avs_analyzer::DetectorPool::Callback;
  using ImagePtr = avs_analyzer::DetectorPool::ImagePtr;
  using AddTaskFunc = avs_analyzer::DetectorPool::AddTaskFunc;
  using SharedInference = avs_analyzer::SharedInference;
  using MailboxTask = avs_analyzer::AnalyzerMailboxes::Task;

  static constexpr unsigned long kStopSentinel =
      std::numeric_limits<unsigned long>::max();

  MockInference(MockInferenceConfig cfg,
                std::shared_ptr<avs_analyzer::ModelMetrics> metrics);
  ~MockInference();

  MockInference(const MockInference&) = delete;
  MockInference& operator=(const MockInference&) = delete;

  /// 对应 DetectorPool::RegisterToDetector：注册回调并返回交给 Analyzer 的
  /// AddTaskFunc。
  AddTaskFunc Register(unsigned long analyzer_id, Callback cb, std::size_t depth);
  void Deregister(unsigned long analyzer_id);

  void Start();  // 仅 queued 模式拉起线程；sync 模式为空操作
  void Stop();

  [[nodiscard]] std::uint64_t submitted() const { return submitted_.load(); }
  [[nodiscard]] std::uint64_t inferred() const { return inferred_.load(); }
  [[nodiscard]] std::uint64_t stale_dropped() const { return stale_dropped_.load(); }
  /// 被 mailbox "最新者胜" 覆盖掉、从未产出的帧数。
  ///
  /// 这是容量不足最直接的信号：生产者投得比消费者快，旧任务在还没被
  /// 推理前就被新任务顶掉了。它**不会**体现在 stale 或 inferred 里，
  /// 属于"静默丢失"，必须单独统计，否则丢帧率会被严重低估。
  [[nodiscard]] std::uint64_t dropped() const { return dropped_.load(); }
  [[nodiscard]] std::uint64_t queue_depth_peak() const { return depth_peak_.load(); }
  /// 最近一次实测的模拟耗时，用于自检注入精度。
  [[nodiscard]] std::uint64_t last_cost_ns() const { return last_cost_ns_.load(); }

 private:
  void AddTask(unsigned long id1, unsigned long id2, unsigned long id3,
               ImagePtr image, std::shared_ptr<SharedInference> shared);
  void RunLoop();
  /// 跑一次"模型"：BurnNs + MakeResults + 指标与计数。sync / queued 共用。
  [[nodiscard]] std::vector<DL_RESULT> Produce(
      const ImagePtr& image, std::shared_ptr<SharedInference>& shared);
  void Deliver(const Callback& cb, std::vector<DL_RESULT>&& results,
               std::uint64_t frame_ts, bool stale);

  /// 确定性产出：同样的 seed + 帧序 => 同样的框。
  [[nodiscard]] std::vector<DL_RESULT> MakeResults(std::uint64_t seq) const;
  void BurnNs(std::uint64_t ns) const;
  std::uint64_t NextRand() const;

  MockInferenceConfig cfg_;
  std::shared_ptr<avs_analyzer::ModelMetrics> metrics_;

  std::mutex cb_mutex_;
  std::unordered_map<unsigned long, Callback> callbacks_;

  avs_analyzer::AnalyzerMailboxes mailboxes_;  // 真实的 mailbox
  std::mutex ready_mutex_;
  std::condition_variable ready_cv_;
  std::deque<unsigned long> ready_ids_;
  std::vector<std::jthread> workers_;
  std::atomic<bool> running_{false};

  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> inferred_{0};
  std::atomic<std::uint64_t> stale_dropped_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> depth_peak_{0};
  std::atomic<std::uint64_t> last_cost_ns_{0};
  mutable std::atomic<std::uint64_t> rng_state_;
  std::atomic<std::uint64_t> seq_{0};
};

}  // namespace avsbench

#endif
