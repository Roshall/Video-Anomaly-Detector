//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_CONSTANT_YOLO_H
#define ANALYZER_CONSTANT_YOLO_H

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "task.h"
#include "yolo_detector_onnx.h"

namespace avs_analyzer {
struct Config;

// Owns N inference sessions for one (model file, target class) pair and
// assigns registered analyzers to sessions round-robin.
class ModelRuntime {
 public:
  ModelRuntime(std::string model_file, bool use_accelerator, int target_id,
               int session_count,
               std::chrono::milliseconds shared_result_wait);

  std::shared_ptr<YOLODetector> Register(unsigned long analyzer_id,
                                         YOLODetector::Callback cb,
                                         size_t depth);
  bool Deregister(unsigned long analyzer_id);
  void StopAll();
  [[nodiscard]] bool isRunning() const;
  [[nodiscard]] size_t session_count() const { return sessions_.size(); }
  [[nodiscard]] const std::string& model_file() const { return model_file_; }
  [[nodiscard]] int target_id() const { return target_id_; }
  [[nodiscard]] std::chrono::milliseconds shared_result_wait() const {
    return shared_result_wait_;
  }

 private:
  std::string model_file_;
  int target_id_;
  std::chrono::milliseconds shared_result_wait_;
  std::vector<std::shared_ptr<YOLODetector>> sessions_;
  std::atomic<size_t> next_session_{0};
  std::mutex registration_mutex_;
  std::unordered_map<unsigned long, size_t> assignments_;
};

class DetectorPool {
 public:
  using Callback = YOLODetector::Callback;
  using ImagePtr = YOLODetector::ImagePtr;
  using SharedInference = YOLODetector::SharedInference;
  using AddTaskFunc = std::function<void(unsigned long id1, unsigned long id2,
                                        unsigned long id3, ImagePtr image,
                                        std::shared_ptr<SharedInference> shared)>;

  explicit DetectorPool(const Config& config);
  ~DetectorPool();

  AddTaskFunc RegisterToDetector(Task task, unsigned long id, Callback cb,
                                 size_t depth = 1);
  bool DeregisterToDetector(Task task, unsigned long id);
  void StopAllDetectors();
  [[nodiscard]] nlohmann::json GetModelStatus() const;

 private:
  std::shared_ptr<ModelRuntime> GetOrCreateRuntime(const std::string& file,
                                                   int target_id);
  static std::string RuntimeKey(const std::string& file, int target_id);

  std::filesystem::path model_path_;
  bool use_accelerator_;
  int workers_per_model_;
  int workers_per_model_accelerated_;
  std::chrono::milliseconds shared_result_wait_;
  mutable std::mutex detector_mutex_;
  std::unordered_map<std::string, std::shared_ptr<ModelRuntime>> runtimes_;
  // Single-flight model loading: concurrent first registrations of the same
  // (file, target) wait on this future instead of loading the model twice.
  std::unordered_map<std::string,
                     std::shared_future<std::shared_ptr<ModelRuntime>>>
      loading_;
};
}  // namespace avs_analyzer

#endif  // ANALYZER_CONSTANT_YOLO_H
