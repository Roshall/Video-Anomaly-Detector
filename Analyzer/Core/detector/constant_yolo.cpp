//
// Created by lg on 10/11/25.
//

#include "constant_yolo.h"

#include <filesystem>

#include "constant_pool.h"
#include "task.h"
#include "yolo_detector_onnx.h"

namespace avs_analyzer{

std::shared_ptr<const YOLODetector> GetDetector(std::string_view model_path,
                                                Task task) {
  static ConstPool<std::string, YOLODetector> pool;
  auto path_prefix = std::filesystem::path(model_path);
  auto task_string{toString(task)};
  switch (task) {
    case Task::kBreakIn:
    case Task::kWandering:
    case Task::kBunkOff:
      return pool.get_or_emplace(task_string,
                                 (path_prefix / "yolov8n.onnx").string());
    case Task::kSleepWork:
      return pool.get_or_emplace(task_string,
                                 (path_prefix / "sleeping_n.onnx").string());
    case Task::kWallClimbing:
      return pool.get_or_emplace(task_string,
                                 (path_prefix / "climbing_m.onnx").string(), 1);
    case Task::kPhonePlaying:
      return pool.get_or_emplace(
          task_string, (path_prefix / "phone_playing_m.onnx").string(), 1);
    default:
      return nullptr;
  }
}
}