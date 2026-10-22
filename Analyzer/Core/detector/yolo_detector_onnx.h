//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_YOLO_DETECTOR_ONNX_H
#define ANALYZER_YOLO_DETECTOR_ONNX_H

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>

#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>

#include "inference.h"
#include "mailbox.h"
#include "metrics.h"

namespace avs_analyzer {
inline constexpr DL_INIT_PARAM kDefaultParam{};

// One inference session. Each session owns its ONNX session and scratch
// buffers and is driven by exactly one worker thread created on demand when
// the first analyzer registers.
class YOLODetector {
 public:
  // Resource identifier: (output_id, video_id, frame_ts)
  using ResourceID = AnalyzerMailboxes::ResourceID;
  using ResultType = std::pair<std::vector<DL_RESULT>, unsigned long>;
  // Callback now consists of the result processor and a predicate to check if the frame is still wanted
  using CallbackFunc = std::function<void(const ResultType&, uint64_t)>;
  using WantsFrameFunc = std::function<uint64_t()>;
  using Callback = std::pair<CallbackFunc, WantsFrameFunc>;
  using ImagePtr = AnalyzerMailboxes::ImagePtr;
  using Task = AnalyzerMailboxes::Task;
  using SharedInference = avs_analyzer::SharedInference;

  explicit YOLODetector(const DL_INIT_PARAM& params = kDefaultParam);
  explicit YOLODetector(std::string_view mode_path,
                        bool use_accelerator = false, int target_id = 0);
  ~YOLODetector();

  void AddTask(Task task);
  // Starts the session thread lazily; `depth` > 1 enables an offline backlog.
  void RegisterCallback(unsigned long id, Callback cb, size_t depth = 1);
  void DeregisterCallback(unsigned long id);
  void Stop();
  [[nodiscard]] bool isRunning() const {
    return running_.load(std::memory_order_acquire);
  }

 private:
  void Start();
  void Run();
  std::vector<DL_RESULT> RunInference(const cv::Mat& image);
  std::vector<DL_RESULT> FilterResults(std::vector<DL_RESULT> input) const;

  DL_INIT_PARAM params_;
  std::unique_ptr<YOLOv8Detect> model_;
  std::atomic<bool> running_{false};
  std::mutex lifecycle_mutex_;

  AnalyzerMailboxes mailboxes_;
  moodycamel::BlockingConcurrentQueue<unsigned long> ready_ids_;

  mutable std::shared_mutex callback_mutex_;
  std::map<unsigned long, Callback> callbacks_;

  std::shared_ptr<ModelMetrics> metrics_;
  std::jthread worker_thread_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_YOLO_DETECTOR_ONNX_H
