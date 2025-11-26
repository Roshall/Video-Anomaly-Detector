//
// Created by lg on 10/11/25.
//

#include "constant_yolo.h"

#include <thread>

#include "yolo_detector_onnx.h"

namespace avs_analyzer {

DetectorPool::DetectorPool(std::string_view model_path, bool use_accelerator)
    : model_path_(model_path), use_accelerator_(use_accelerator) {}

DetectorPool::~DetectorPool() {
  StopAllDetectors();
}

std::shared_ptr<const YOLODetector> DetectorPool::RegisterToDetector(
    Task task, unsigned long id1, unsigned long id2, Callback cb) {
  auto task_string = toString(task);

  std::shared_ptr<const YOLODetector> detector;
  switch (task) {
    case Task::kBreakIn:
    case Task::kWandering:
    case Task::kBunkOff:
      detector = detector_pool_.get_or_emplace(
          task_string, (model_path_ / "yolov8n.onnx").string(), use_accelerator_);
      break;
    case Task::kSleepWork:
      detector = detector_pool_.get_or_emplace(
          task_string, (model_path_ / "sleeping_m.onnx").string(), use_accelerator_);
      break;
    case Task::kWallClimbing:
      detector = detector_pool_.get_or_emplace(
          task_string, (model_path_ / "climbing_m.onnx").string(), use_accelerator_, 1);
      break;
    case Task::kPhonePlaying:
      detector = detector_pool_.get_or_emplace(
          task_string, (model_path_ / "phone_playing_m.onnx").string(), use_accelerator_, 1);
      break;
    default:
      return nullptr;
  }

  if (detector) {
    // Register the callback for this resource
    const_cast<YOLODetector*>(detector.get())->RegisterCallback(id1, id2, std::move(cb));

    // Launch worker thread if detector is not running
    if (!detector->isRunning()) {
      std::thread([detector]() {
        const_cast<YOLODetector*>(detector.get())->Run();
      }).detach();
    }
  }

  return detector;
}

void DetectorPool::StopAllDetectors() {
  auto detectors = detector_pool_.all();
  for (auto& detector : detectors) {
    const_cast<YOLODetector*>(detector.get())->Stop();
  }
}

// Deprecated shim for legacy sync callers
static ConstPool<std::string, YOLODetector> g_legacy_pool;

std::shared_ptr<const YOLODetector> GetDetector(
    std::string_view model_path, Task task, bool use_accelerator) {
  auto path_prefix = std::filesystem::path(model_path);
  auto task_string = toString(task);

  std::shared_ptr<const YOLODetector> detector;
  switch (task) {
    case Task::kBreakIn:
    case Task::kWandering:
    case Task::kBunkOff:
      detector = g_legacy_pool.get_or_emplace(
          task_string, (path_prefix / "yolov8n.onnx").string(), use_accelerator);
      break;
    case Task::kSleepWork:
      detector = g_legacy_pool.get_or_emplace(
          task_string, (path_prefix / "sleeping_m.onnx").string(), use_accelerator);
      break;
    case Task::kWallClimbing:
      detector = g_legacy_pool.get_or_emplace(
          task_string, (path_prefix / "climbing_m.onnx").string(), use_accelerator, 1);
      break;
    case Task::kPhonePlaying:
      detector = g_legacy_pool.get_or_emplace(
          task_string, (path_prefix / "phone_playing_m.onnx").string(), use_accelerator, 1);
      break;
    default:
      return nullptr;
  }
  return detector;
}

}  // namespace avs_analyzer