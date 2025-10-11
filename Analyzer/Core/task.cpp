//
// Created by lg on 10/11/25.
//
#include "task.h"
#include <string>

std::string avs_analyzer::toString(Task task) {
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
