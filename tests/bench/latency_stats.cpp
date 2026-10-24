#include "latency_stats.h"

#include <algorithm>
#include <cmath>

namespace avsbench {

LatencyStats::Summary LatencyStats::Summarize() const {
  Summary s;
  s.count = samples_.size();
  if (samples_.empty()) return s;

  std::vector<std::int64_t> v(samples_);
  std::sort(v.begin(), v.end());

  s.min_ns = v.front();
  s.max_ns = v.back();

  double sum = 0.0;
  for (const auto x : v) sum += static_cast<double>(x);
  s.mean_ns = sum / static_cast<double>(v.size());

  double sq = 0.0;
  for (const auto x : v) {
    const double d = static_cast<double>(x) - s.mean_ns;
    sq += d * d;
  }
  s.stdev_ns = std::sqrt(sq / static_cast<double>(v.size()));

  // 线性插值取分位：比取最近秩更稳定，尤其在样本少的时候。
  auto percentile = [&v](double q) -> std::int64_t {
    const double pos = q * (static_cast<double>(v.size()) - 1.0);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = std::min(lo + 1, v.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    const double a = static_cast<double>(v[lo]);
    const double b = static_cast<double>(v[hi]);
    return static_cast<std::int64_t>(a + (b - a) * frac);
  };

  s.p50_ns = percentile(0.50);
  s.p95_ns = percentile(0.95);
  s.p99_ns = percentile(0.99);
  s.p999_ns = percentile(0.999);
  return s;
}

}  // namespace avsbench
