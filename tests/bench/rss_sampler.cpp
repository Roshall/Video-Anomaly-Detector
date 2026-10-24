#include "rss_sampler.h"

#include <malloc.h>
#include <sys/resource.h>

#include <fcntl.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace avsbench {

RssSampler::RssSampler(std::size_t reserve_count) { points_.reserve(reserve_count); }

std::uint64_t RssSampler::CurrentRssBytes() {
  const int fd = ::open("/proc/self/statm", O_RDONLY);
  if (fd < 0) return CurrentRssBytesStatus();

  char buf[256];
  const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  if (n <= 0) return CurrentRssBytesStatus();
  buf[n] = '\0';

  // statm: total resident shared text lib data dt
  long total = 0;
  long resident = 0;
  if (std::sscanf(buf, "%ld %ld", &total, &resident) != 2) {
    return CurrentRssBytesStatus();
  }
  const long page = ::sysconf(_SC_PAGESIZE);
  if (resident <= 0 || page <= 0) return 0;
  return static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(page);
}

std::uint64_t RssSampler::CurrentRssBytesStatus() {
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (!f) return 0;

  char line[256];
  std::uint64_t rss = 0;
  while (std::fgets(line, sizeof(line), f)) {
    if (std::strncmp(line, "VmRSS:", 6) != 0) continue;
    long kb = 0;
    if (std::sscanf(line + 6, "%ld", &kb) == 1 && kb > 0) {
      rss = static_cast<std::uint64_t>(kb) * 1024u;
    }
    break;
  }
  std::fclose(f);
  return rss;
}

std::uint64_t RssSampler::PeakRssBytes() {
  struct rusage ru {};
  if (::getrusage(RUSAGE_SELF, &ru) != 0) return 0;
  // ru_maxrss 在 Linux 上以 KB 计。
  return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024u;
}

void RssSampler::TrimHeap() { ::malloc_trim(0); }

void RssSampler::Sample(std::uint64_t frame_index, std::int64_t wall_ms) {
  const auto rss = CurrentRssBytes();
  // 内核的 ru_maxrss 更新可能滞后于实际驻留（实测会出现 peak < current），
  // 所以这里取两者最大值：既保留高水位语义，又保证 peak >= current 恒成立，
  // 下游的 peak - final 不会出现负值。
  const auto peak = std::max(PeakRssBytes(), rss);

  RssPoint p;
  p.frame_index = frame_index;
  p.wall_ms = wall_ms;
  p.rss_bytes = rss;
  p.peak_bytes = peak;
  points_.push_back(p);
}

double RssSampler::SlopeBytesPerFrame(std::uint64_t from_frame,
                                      std::uint64_t to_frame) const {
  double sx = 0.0;
  double sy = 0.0;
  double sxx = 0.0;
  double sxy = 0.0;
  std::size_t n = 0;

  for (const auto& p : points_) {
    if (p.frame_index < from_frame || p.frame_index >= to_frame) continue;
    const double x = static_cast<double>(p.frame_index);
    const double y = static_cast<double>(p.rss_bytes);
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
    ++n;
  }

  if (n < 2) return 0.0;
  const double denom = static_cast<double>(n) * sxx - sx * sx;
  if (std::fabs(denom) < 1e-9) return 0.0;
  return (static_cast<double>(n) * sxy - sx * sy) / denom;
}

}  // namespace avsbench
