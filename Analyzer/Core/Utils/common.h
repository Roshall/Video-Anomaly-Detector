#ifndef ANALYZER_COMMON_H
#define ANALYZER_COMMON_H

#include <chrono>
#include <string>
#include <vector>

namespace avs_analyzer {
static int64_t getCurTimestamp()  // 获取毫秒级时间戳（13位）
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

static std::vector<std::string> split(const std::string& str,
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



};  // namespace avs_analyzer

#endif  // ANALYZER_COMMON_H