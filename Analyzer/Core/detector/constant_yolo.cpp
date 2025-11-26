//
// Created by lg on 10/11/25.
//

#include "constant_yolo.h"

#include <thread>

#include "Utils/enum_utils.h"
#include "yolo_detector_onnx.h"

namespace avs_analyzer {

DetectorPool::DetectorPool(std::string_view model_path, bool use_accelerator)
    : model_path_(model_path), use_accelerator_(use_accelerator) {}

DetectorPool::~DetectorPool() {
  StopAllDetectors();
}

std::shared_ptr<const YOLODetector> DetectorPool::RegisterToDetector(
  Task task, unsigned long id1, unsigned long id2, Callback cb) {

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
      return nullptr;
  }
  auto detector = detector_pool_.get_or_emplace(
          task, (model_path_ / model_name).string(), use_accelerator_);

  if (detector) {
    // Register the callback for this resource
    const_cast<YOLODetector*>(detector.get())->RegisterCallback(id1, id2, std::move(cb));

    // Launch worker thread if not already running
    int idx = enum_order(task);
    if (idx >= 0) {
      std::lock_guard<std::mutex> lock(threads_mutex_);
      if (!threads_[idx].joinable()) {
        threads_[idx] = std::thread([detector]() {
          const_cast<YOLODetector*>(detector.get())->Run();
        });
      }
    }
  }

  return detector;
}

bool DetectorPool::RemoveDetector(Task task) {
  auto detector = detector_pool_.get(task);
  if (!detector) {
    return false;  // Not in pool
  }

  // If still running, cannot remove
  if (detector->isRunning()) {
    return false;
  }

  // Join the thread if joinable
  int idx = enum_order(task);
  if (idx >= 0) {
    std::unique_lock<std::mutex> lock(threads_mutex_);
    if (threads_[idx].joinable()) {
      // Unlock while joining to avoid blocking other operations
      std::thread t = std::move(threads_[idx]);
      lock.unlock();
      t.join();
    }
  }

  // Erase from pool
  return detector_pool_.erase(task);
}

void DetectorPool::StopAllDetectors() {
  // Stop all detectors first
  auto detectors = detector_pool_.all();
  for (auto& detector : detectors) {
    const_cast<YOLODetector*>(detector.get())->Stop();
  }

  // Join all threads
  std::lock_guard<std::mutex> lock(threads_mutex_);
  for (auto& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
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