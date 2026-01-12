#ifndef ANALYZER_ANALYZER_H
#define ANALYZER_ANALYZER_H
#include <opencv2/core/hal/interface.h>
#include <atomic>
#include <cstdint>
#include <limits>
#include <opencv2/opencv.hpp>
#include <readerwriterqueue.h>
#include <memory>
#include <vector>
#include "detector/constant_yolo.h"
#include "task.h"
#include "algorithm.h"

namespace avs_analyzer {
struct Monitor;
struct Config;
class Scheduler;
struct Detection;

class Analyzer {
 public:
  Analyzer(Monitor* monitor, const Config& config);
  virtual ~Analyzer();

  using ImagePtr = DetectorPool::ImagePtr;
  void AddVideoFrame(uint64_t video_id, uint64_t frame_id, ImagePtr image);
  [[nodiscard]] virtual std::vector<cv::Rect> RetrieveDetection() const = 0;
  void SetNextFrameTimestamp(uint64_t ts) {
    frame_ts_ms_.store(ts, std::memory_order_release);
  }
  [[nodiscard]] unsigned long id() const { return id_; }
  [[nodiscard]] Task task() const { return task_; }
  [[nodiscard]] DetectorPool::Callback detector_callback() const {
    return {[algorithm = algorithm_.get()](const YOLODetector::ResultType& input,
                                           uint64_t frame_ts) {
              algorithm->PushResult(input, frame_ts);
            },
            [this]() { return WantsFrame(); }};
  }
  void SetAddTaskCallback(DetectorPool::AddTaskFunc cb) {
    add_task_callback_ = std::move(cb);
  }
  virtual void SetROIs(const std::vector<std::vector<cv::Point>>& polygons) = 0;
  uint64_t WantsFrame() const {
    return frame_ts_ms_.load(std::memory_order_acquire);
  }

  void Stop() {
    // Push an empty result to unblock FetchOneFrameResults
    algorithm_->PushResult({{}, 0}, std::numeric_limits<uint64_t>::max());
  }

 protected:
  [[nodiscard]] Detection FetchRawDetection() const;
  [[nodiscard]] float conf_threshold() const { return conf_th_; }

 private:
  static uint64_t NextId();

  uint64_t id_{NextId()};
  std::atomic<uint64_t> frame_ts_ms_{0};
  Task task_;
  std::unique_ptr<Algorithm> algorithm_;
  float conf_th_;
  DetectorPool::AddTaskFunc add_task_callback_;
};

std::unique_ptr<Analyzer> CreateAnalyzer(Monitor* monitor, const Config& config);
}  // namespace avs_analyzer
#endif  // ANALYZER_ANALYZER_H
