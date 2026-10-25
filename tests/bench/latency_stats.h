// 延迟统计：热路径零分配地收集样本，收尾时算分位数。
//
// 为什么自己算而不用现成的直方图：这里要的是"逐帧端到端"的 p99，
// 且样本量在十万级，直接 reserve + sort + 线性插值最稳（nth_element
// 要为 4 个分位各调一次，反而更慢更不稳）。
#ifndef AVS_BENCH_LATENCY_STATS_H
#define AVS_BENCH_LATENCY_STATS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace avsbench {

class LatencyStats {
 public:
  void Reserve(std::size_t n) { samples_.reserve(n); }

  /// 热路径：仅在已 reserve 的 vector 上 push_back。
  void Add(std::int64_t ns) { samples_.push_back(ns); }

  [[nodiscard]] std::uint64_t count() const { return samples_.size(); }
  [[nodiscard]] bool empty() const { return samples_.empty(); }
  /// 原始样本，供离线画直方图/导出 CSV。
  [[nodiscard]] const std::vector<std::int64_t>& samples() const {
    return samples_;
  }

  struct Summary {
    std::uint64_t count{0};
    std::int64_t min_ns{0};
    std::int64_t max_ns{0};
    std::int64_t p50_ns{0};
    std::int64_t p95_ns{0};
    std::int64_t p99_ns{0};
    std::int64_t p999_ns{0};
    double mean_ns{0.0};
    double stdev_ns{0.0};
  };

  [[nodiscard]] Summary Summarize() const;

 private:
  std::vector<std::int64_t> samples_;
};

/// 分阶段埋点：端到端是主指标，其余用于定位瓶颈落在哪一段。
struct LatencyBuckets {
  LatencyStats convert_ns;     // YUV -> BGR（真实 FormatConvertor）
  LatencyStats preprocess_ns;  // BGR -> 模型张量（真实 OpenCvImageProcessor）
  LatencyStats submit_ns;      // AddVideoFrame（入队）
  LatencyStats wait_ns;        // ProcessFrame 内等结果 + 算法后处理
  LatencyStats e2e_ns;         // 主指标 = convert + preprocess + submit + wait
  LatencyStats mock_ns;        // mock 内部实测耗时，自检注入精度
  LatencyStats event_ns;       // 发生了 stride check 的帧

  void ReserveAll(std::size_t n) {
    convert_ns.Reserve(n);
    preprocess_ns.Reserve(n);
    submit_ns.Reserve(n);
    wait_ns.Reserve(n);
    e2e_ns.Reserve(n);
    mock_ns.Reserve(n);
    event_ns.Reserve(n);
  }
};

}  // namespace avsbench

#endif
