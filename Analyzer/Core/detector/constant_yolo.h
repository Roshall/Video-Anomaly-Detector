//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_CONSTANT_YOLO_H
#define ANALYZER_CONSTANT_YOLO_H

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>

#include "constant_pool.h"
#include "yolo_detector_onnx.h"
#include "task.h"

namespace avs_analyzer {
class YOLODetector;

class DetectorPool {
 public:
  using Callback = YOLODetector::Callback;
  DetectorPool(std::string_view model_path, bool use_accelerator);
  using AddTaskFunc = std::function<void(unsigned long id1, unsigned long id2, unsigned long id3, const cv::Mat& image)>;
  ~DetectorPool();

  // Get or create detector for the task, register callback, and return handle
    // Register a detector for `task` and callback; returns an Adder callable to push tasks.
  AddTaskFunc RegisterToDetector(Task task, unsigned long id, Callback cb);

  // Remove detector if it has stopped running
  bool RemoveDetector(Task task, bool force=true);
  // Deregister a previously registered callback from a detector and try to remove it
  // Returns true if deregistration succeeded and the detector was removed; false otherwise.
  bool DeregisterToDetector(Task task, unsigned long id);

  void StopAllDetectors();

 private:
  std::filesystem::path model_path_;
  bool use_accelerator_;
  ConstPool<Task, YOLODetector> detector_pool_;
  std::mutex detector_mutex_;
};

// Deprecated: legacy helper for sync callers—will be removed
[[deprecated("Use DetectorPool::RegisterToDetector instead")]]
std::shared_ptr<const YOLODetector> GetDetector(
    std::string_view model_path, Task task, bool use_accelerator);

}  // namespace avs_analyzer

#endif  // ANALYZER_CONSTANT_YOLO_H
