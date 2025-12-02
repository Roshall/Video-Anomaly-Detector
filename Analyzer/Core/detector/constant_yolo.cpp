//
// Created by lg on 10/11/25.
//

#include "constant_yolo.h"

#include <memory>

#include "yolo_detector_onnx.h"
#include "Utils/log.h"

namespace avs_analyzer {

DetectorPool::DetectorPool(std::string_view model_path, bool use_accelerator)
    : model_path_(model_path), use_accelerator_(use_accelerator) {}

DetectorPool::~DetectorPool() {
  StopAllDetectors();
}

DetectorPool::AddTaskFunc DetectorPool::RegisterToDetector(Task task, unsigned long id, Callback cb) {
  std::string model_name;
  switch (task) {
    case Task::kBreakIn:
    case Task::kWandering:
    case Task::kBunkOff:
      model_name = "yolov8n.onnx";
      break;
    case Task::kSleepWork:
      model_name = "sleeping_m.onnx";
      break;
    case Task::kWallClimbing:
      model_name = "climbing_m.onnx";
      break;
    case Task::kPhonePlaying:
      model_name = "phone_playing_m.onnx";
      break;
    default:
      return [](unsigned long, unsigned long, unsigned long, const cv::Mat&) {};
  }
  std::unique_lock<std::mutex> lock(detector_mutex_);
  auto detector = detector_pool_.get_or_emplace(
      task, (model_path_ / model_name).string(), use_accelerator_);

  if (!detector) {
    // Return a no-op adder
    LOGE("Failed to create or get detector for task {}", toString(task));
    return [](unsigned long, unsigned long, unsigned long, const cv::Mat&) {};
  } else{
    // Register the callback for this resource
    const_cast<YOLODetector*>(detector.get())->RegisterCallback(id, std::move(cb));
  }

  // Return callable for enqueuing tasks
  return [detector](unsigned long id1, unsigned long id2, unsigned long id3, const cv::Mat& image) {
    const_cast<YOLODetector*>(detector.get())->AddTask({{id1, id2, id3}, image});
  };
}

bool DetectorPool::RemoveDetector(Task task, bool force) {
  if (force) {
    auto detector = detector_pool_.get(task);
    if (!detector) {
      return false;  // Not in pool
    }

    // If still running, cannot remove
    if (detector->isRunning()) {
      return false;
    }
  }
  // Erase from pool
  return detector_pool_.erase(task);
}

bool DetectorPool::DeregisterToDetector(Task task, unsigned long id) {
  std::unique_lock<std::mutex> lock(detector_mutex_);
  auto detector = detector_pool_.get(task);
  if (!detector) return false;

  // Call into detector to remove the callback -- this will stop the detector if
  // it's now empty.
  const_cast<YOLODetector*>(detector.get())->DeregisterCallback(id);

  // If the detector has already stopped, try to remove it from the pool.
  if (!detector->isRunning()) {
    return detector_pool_.erase(task);
  }
  return true;  // deregistered, but the detector still runs
}

void DetectorPool::StopAllDetectors() {
  // Stop all detectors first
  std::lock_guard<std::mutex> lock(detector_mutex_);
  auto detectors = detector_pool_.all();
  for (auto& detector : detectors) {
    const_cast<YOLODetector*>(detector.get())->Stop();
  }
}

}  // namespace avs_analyzer