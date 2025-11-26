//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_YOLO_DETECTOR_ONNX_H
#define ANALYZER_YOLO_DETECTOR_ONNX_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <semaphore>
#include <shared_mutex>
#include <utility>

#include <concurrentqueue/moodycamel/concurrentqueue.h>

#include "inference.h"

namespace avs_analyzer {
inline constexpr DL_INIT_PARAM kDefaultParam{};

class YOLODetector {
 public:
  using ResourceID = std::pair<unsigned long, unsigned long>;
  using Callback = std::function<void(const std::vector<DL_RESULT>&)>;

  explicit YOLODetector(const DL_INIT_PARAM& params = kDefaultParam);
  explicit YOLODetector(std::string_view mode_path,
                        bool use_accelerator = false, int target_id = 0);

  void AddTask(unsigned long id1, unsigned long id2, const cv::Mat& image);
  void RegisterCallback(unsigned long id1, unsigned long id2, Callback cb);
    void DeregisterCallback(unsigned long id1, unsigned long id2);
  void Run();
  void Stop();
  [[nodiscard]] bool isRunning() const { return running_.load(); }

 private:
  struct Task {
    ResourceID id;
    cv::Mat image;
  };

  void InvokeCallback(const ResourceID& id, const std::vector<DL_RESULT>& results) const;
  void DropOverflowTasks();
  std::vector<DL_RESULT> FilterResults(std::vector<DL_RESULT> input) const;

  DL_INIT_PARAM params_;
  std::unique_ptr<YOLOv8Detect> model_;
  moodycamel::ConcurrentQueue<Task> task_queue_;
  std::counting_semaphore<> task_sem_{0};
  std::map<ResourceID, Callback> callbacks_;
  mutable std::shared_mutex callback_mutex_;
  std::atomic<bool> running_{true};
  static constexpr size_t kMaxQueueSize = 100;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_YOLO_DETECTOR_ONNX_H
