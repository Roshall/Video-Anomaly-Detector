// RSS 采样：区分"当前驻留"与"历史峰值"，用于判定内存泄漏。
//
// 两个数据源必须配合：
//   - /proc/self/statm  → 当前驻留页数（权威的"现在占多少"）
//   - getrusage ru_maxrss → 历史峰值（statm 拿不到；peak - current 能暴露
//     "中途冲高后回落"的瞬时堆积，那也是问题，只是不体现在终值上）
#ifndef AVS_BENCH_RSS_SAMPLER_H
#define AVS_BENCH_RSS_SAMPLER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace avsbench {

struct RssPoint {
  std::uint64_t frame_index{0};
  std::int64_t wall_ms{0};
  std::uint64_t rss_bytes{0};   // 当前驻留
  std::uint64_t peak_bytes{0};  // 历史峰值
};

class RssSampler {
 public:
  explicit RssSampler(std::size_t reserve_count);

  /// 当前驻留字节数。首选 /proc/self/statm（一次 open+read，2~5 us）。
  static std::uint64_t CurrentRssBytes();
  /// statm 不可用时的回退：解析 /proc/self/status 的 VmRSS。
  static std::uint64_t CurrentRssBytesStatus();
  /// 历史峰值（getrusage ru_maxrss，Linux 上单位是 KB）。
  static std::uint64_t PeakRssBytes();
  /// malloc_trim(0)：把已 free 但未归还 OS 的堆还回去，让"真泄漏"从
  /// 分配器水位里浮出来。只在阶段边界调用，绝不在逐帧路径上。
  static void TrimHeap();

  void Sample(std::uint64_t frame_index, std::int64_t wall_ms);

  [[nodiscard]] const std::vector<RssPoint>& points() const { return points_; }
  [[nodiscard]] std::size_t size() const { return points_.size(); }

  /// 对 [from_frame, to_frame) 做最小二乘，返回 bytes/frame。
  /// 正常缓存增长会在稳态段收敛到 ~0；真泄漏则是一个稳定的正斜率。
  [[nodiscard]] double SlopeBytesPerFrame(std::uint64_t from_frame,
                                          std::uint64_t to_frame) const;

 private:
  std::vector<RssPoint> points_;
};

}  // namespace avsbench

#endif
