//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_CONSTANT_YOLO_H
#define ANALYZER_CONSTANT_YOLO_H

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "constant_pool.h"
#include "inference.h"
#include "task.h"

namespace avs_analyzer {
class YOLODetector;

class DetectorPool {
 public:
  using Callback = std::function<void(const std::vector<DL_RESULT>&)>;

  DetectorPool(std::string_view model_path, bool use_accelerator);
  ~DetectorPool();

  // Get or create detector for the task, register callback, and return handle
  std::shared_ptr<const YOLODetector> RegisterToDetector(
      Task task, unsigned long id1, unsigned long id2, Callback cb);

  void StopAllDetectors();

 private:
  std::filesystem::path model_path_;
  bool use_accelerator_;
  ConstPool<std::string, YOLODetector> detector_pool_;
};

// Deprecated: legacy helper for sync callers—will be removed
[[deprecated("Use DetectorPool::RegisterToDetector instead")]]
std::shared_ptr<const YOLODetector> GetDetector(
    std::string_view model_path, Task task, bool use_accelerator);

}  // namespace avs_analyzer

#endif  // ANALYZER_CONSTANT_YOLO_H
