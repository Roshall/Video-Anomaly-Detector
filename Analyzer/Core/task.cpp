//
// Created by lg on 10/11/25.
//
#include "task.h"
#include <string>

std::string avs_analyzer::toString(const Task task) {
  switch (task) {
    case Task::kPhonePlaying:
      return "PhonePlaying";
    case Task::kSleepWork:
      return "SleepWork";
    case Task::kWandering:
      return "Wandering";
    case Task::kBreakIn:
      return "BreakIn";
    case Task::kWallClimbing:
      return "kWallClimbing";
    case Task::kBunkOff:
      return "BunkOff";
    default:
      return "Unknown Task";
  }
}
std::u8string avs_analyzer::toChinese(const Task task) {
  switch (task) {
    case Task::kPhonePlaying:
      return u8"玩手机";
    case Task::kSleepWork:
      return u8"睡岗";
    case Task::kWandering:
      return u8"逗留";
    case Task::kBreakIn:
      return u8"闯入";
    case Task::kWallClimbing:
      return u8"攀爬";
    case Task::kBunkOff:
      return u8"离岗";
    default:
      return u8"未定义";
  }
}
