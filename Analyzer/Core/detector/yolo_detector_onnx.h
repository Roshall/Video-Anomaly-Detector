//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_YOLO_DETECTOR_ONNX_H
#define ANALYZER_YOLO_DETECTOR_ONNX_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <thread>
#include <utility>

#include <concurrentqueue/moodycamel/concurrentqueue.h>

#include "inference.h"

namespace avs_analyzer {
inline constexpr DL_INIT_PARAM kDefaultParam{};

class YOLODetector {
 public:
  // Resource identifier: (output_id, video_id, frame_ts)
  using ResourceID = std::array<unsigned long, 3>;
  using ResultType = std::pair<std::vector<DL_RESULT>, unsigned long>;
  // Callback now receives the detection result and the original frame timestamp (frame id)
  using Callback = std::function<void(const ResultType&, uint64_t)>;
  using ImagePtr = std::shared_ptr<const cv::Mat>;
  struct Task {
    ResourceID id;
    ImagePtr image;
  };

  explicit YOLODetector(const DL_INIT_PARAM& params = kDefaultParam);
  explicit YOLODetector(std::string_view mode_path,
                        bool use_accelerator = false, int target_id = 0);
  ~YOLODetector();

  void AddTask(Task task);
  void RegisterCallback(unsigned long id, Callback cb);
  void DeregisterCallback(unsigned long id);
  // Asynchronous consumer loop (run in dedicated thread)
  void Run();
  void Stop();
  [[nodiscard]] bool isRunning() const { return running_.load(); }

 private:
  void InvokeCallback(const ResourceID& id, const std::vector<DL_RESULT>& results) const;
  void DropOverflowTasks();
  std::vector<DL_RESULT> FilterResults(std::vector<DL_RESULT> input) const;

  DL_INIT_PARAM params_;
  std::unique_ptr<YOLOv8Detect> model_;
  mutable std::mutex mutex_;
  moodycamel::ConcurrentQueue<Task> task_queue_;
  std::counting_semaphore<> task_sem_{0};
  std::map<unsigned long, Callback> callbacks_;
  std::atomic<bool> running_{true};
  mutable std::shared_mutex callback_mutex_;
  static constexpr size_t kMaxQueueSize = 100;
  std::jthread worker_thread_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_YOLO_DETECTOR_ONNX_H
