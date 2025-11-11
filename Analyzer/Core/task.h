//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_TASK_H
#define ANALYZER_TASK_H
#include <string>

namespace avs_analyzer{
enum class Task {
  kPhonePlaying,
  kSleepWork,
  kWandering,
  kBreakIn,
  kWallClimbing,
  kBunkOff
};

std::string toString(Task task);
std::u8string toChinese(Task task);
}
#endif  // ANALYZER_TASK_H
