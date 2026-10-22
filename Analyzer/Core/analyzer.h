#ifndef ANALYZER_ANALYZER_H
#define ANALYZER_ANALYZER_H
#include <opencv2/core/hal/interface.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <opencv2/opencv.hpp>
#include <readerwriterqueue.h>
#include <memory>
#include <vector>
#include "detector/constant_yolo.h"
#include "metrics.h"
#include "task.h"
#include "algorithm.h"

namespace avs_analyzer {
struct Monitor;
struct Config;
class Scheduler;
struct Detection;

class Analyzer : public std::enable_shared_from_this<Analyzer> {
 public:
  Analyzer(Monitor* monitor, const Config& config);
  virtual ~Analyzer();

  using ImagePtr = DetectorPool::ImagePtr;
  using SharedInference = DetectorPool::SharedInference;
  void AddVideoFrame(uint64_t video_id, uint64_t frame_id, ImagePtr image,
                     std::shared_ptr<SharedInference> shared = nullptr);
  [[nodiscard]] virtual std::vector<cv::Rect> RetrieveDetection() const = 0;
  void SetNextFrameTimestamp(uint64_t ts) {
    frame_ts_ms_.store(ts, std::memory_order_release);
  }
  [[nodiscard]] unsigned long id() const { return id_; }
  [[nodiscard]] Task task() const { return task_; }
  [[nodiscard]] DetectorPool::Callback detector_callback() {
    // Inference sessions can outlive the analyzer for a moment during
    // shutdown. Hold the analyzer weakly so an in-flight result can never
    // touch an algorithm that is already being destroyed.
    std::weak_ptr<Analyzer> weak = weak_from_this();
    return {[weak](const YOLODetector::ResultType& input, uint64_t frame_ts) {
              if (auto self = weak.lock()) {
                self->algorithm_->PushResult(input, frame_ts);
              }
            },
            [weak]() -> uint64_t {
              if (auto self = weak.lock()) {
                return self->WantsFrame();
              }
              // Everything is stale once the analyzer is gone.
              return std::numeric_limits<uint64_t>::max();
            }};
  }
  void SetAddTaskCallback(DetectorPool::AddTaskFunc cb) {
    add_task_callback_ = std::move(cb);
  }
  void SetMetrics(std::shared_ptr<WorkerMetrics> metrics) {
    metrics_ = std::move(metrics);
  }
  virtual void SetROIs(const std::vector<std::vector<cv::Point>>& polygons) = 0;
  uint64_t WantsFrame() const {
    return frame_ts_ms_.load(std::memory_order_acquire);
  }

  void Stop() {
    stopped_.store(true, std::memory_order_release);
    // Wake a thread blocked waiting for the next inference result.
    algorithm_->PushResult({{}, 0}, std::numeric_limits<uint64_t>::max());
  }

 protected:
  [[nodiscard]] Detection FetchRawDetection() const;
  [[nodiscard]] float conf_threshold() const { return conf_th_; }

 private:
  static uint64_t NextId();

  uint64_t id_{NextId()};
  std::atomic<uint64_t> frame_ts_ms_{0};
  std::atomic<bool> stopped_{false};
  Task task_;
  std::unique_ptr<Algorithm> algorithm_;
  float conf_th_;
  std::chrono::milliseconds result_timeout_{5000};
  mutable std::atomic<int64_t> last_timeout_log_ms_{0};
  DetectorPool::AddTaskFunc add_task_callback_;
  std::shared_ptr<WorkerMetrics> metrics_;
};

std::unique_ptr<Analyzer> CreateAnalyzer(Monitor* monitor, const Config& config);
}  // namespace avs_analyzer
#endif  // ANALYZER_ANALYZER_H
