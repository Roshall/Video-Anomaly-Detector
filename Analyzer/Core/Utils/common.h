#ifndef ANALYZER_COMMON_H
#define ANALYZER_COMMON_H

#include <string>
#include <vector>

namespace avs_analyzer {
int64_t getCurTimestamp();

std::vector<std::string> split(const std::string& str,
                                      const std::string& sep);
};  // namespace avs_analyzer

#endif  // ANALYZER_COMMON_H