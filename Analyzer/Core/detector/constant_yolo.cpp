//
// Created by lg on 10/11/25.
//

#include "constant_yolo.h"

#include <filesystem>
#include <thread>

#include "constant_pool.h"
#include "task.h"
#include "yolo_detector_onnx.h"

namespace avs_analyzer {
static ConstPool<std::string, YOLODetector> g_detector_pool;

std::shared_ptr<const YOLODetector> GetDetector(std::string_view model_path,
                                                Task task,
                                                bool use_accelerator) {
  auto path_prefix = std::filesystem::path(model_path);
  auto task_string{toString(task)};
  
  std::shared_ptr<const YOLODetector> detector;
  switch (task) {
    case Task::kBreakIn:
    case Task::kWandering:
    case Task::kBunkOff:
      detector = g_detector_pool.get_or_emplace(task_string,
                                                (path_prefix / "yolov8n.onnx").string(), use_accelerator);
      break;
    case Task::kSleepWork:
      detector = g_detector_pool.get_or_emplace(task_string,
                                               (path_prefix / "sleeping_m.onnx").string(), use_accelerator);
      break;
    case Task::kWallClimbing:
      detector = g_detector_pool.get_or_emplace(task_string,
                                               (path_prefix / "climbing_m.onnx").string(), use_accelerator, 1);
      break;
    case Task::kPhonePlaying:
      detector = g_detector_pool.get_or_emplace(
          task_string, (path_prefix / "phone_playing_m.onnx").string(), use_accelerator, 1);
      break;
    default:
      return nullptr;
  }

  // Launch worker thread if detector is not running
  if (detector && !detector->isRunning()) {
    std::thread([detector]() { const_cast<YOLODetector*>(detector.get())->Run(); }).detach();
  }

  return detector;
}
void StopAllDetectors() {
  auto detectors = g_detector_pool.all();
  for (auto& detector : detectors) {
    const_cast<YOLODetector*>(detector.get())->Stop();
  }
}

void ClearYoloDetectorPool() {
  g_detector_pool.clear();
}
}  // namespace avs_analyzer