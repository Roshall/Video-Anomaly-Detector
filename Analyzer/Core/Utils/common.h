#ifndef ANALYZER_COMMON_H
#define ANALYZER_COMMON_H

#include <string>
#include <vector>

namespace avs_analyzer {
int64_t getCurTimestamp();

// Monotonic, process-unique alarm id: max(now_ms, last + 1).
int64_t NextAlarmId();

std::vector<std::string> split(const std::string& str,
                                      const std::string& sep);
long getFileSize(const std::string& filename);
};  // namespace avs_analyzer

#endif  // ANALYZER_COMMON_H