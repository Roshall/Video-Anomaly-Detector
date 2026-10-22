#include "Utils/common.h"
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>

namespace  avs_analyzer {
std::vector<std::string> split(const std::string& str,
                                             const std::string& sep) {
  std::vector<std::string> arr;
  int sepSize = sep.size();

  int lastPosition = 0, index = -1;
  while (-1 != (index = str.find(sep, lastPosition))) {
    arr.push_back(str.substr(lastPosition, index - lastPosition));
    lastPosition = index + sepSize;
  }
  std::string lastStr = str.substr(lastPosition);  // 截取最后一个分隔符后的内容

  if (!lastStr.empty()) {
    arr.push_back(lastStr);  // 如果最后一个分隔符后还有内容就入队
  }

  return arr;
}

long getFileSize(const std::string& filename) {
    struct stat st;
    if (stat(filename.c_str(), &st) != 0) {
        return -1;
    }
    return st.st_size;
}

int64_t getCurTimestamp()  // 获取毫秒级时间戳（13位）
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t NextAlarmId() {
  static std::atomic<int64_t> last{0};
  int64_t prev = last.load(std::memory_order_relaxed);
  for (;;) {
    const int64_t next = std::max(getCurTimestamp(), prev + 1);
    if (last.compare_exchange_weak(prev, next, std::memory_order_relaxed)) {
      return next;
    }
  }
}
}