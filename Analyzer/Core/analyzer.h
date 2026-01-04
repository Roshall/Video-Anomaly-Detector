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
  ~Analyzer();

  using ImagePtr = DetectorPool::ImagePtr;
  void AddVideoFrame(uint64_t video_id, uint64_t frame_id, ImagePtr image);
  [[nodiscard]] std::vector<cv::Rect> RetrieveDetection() const;
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
  void SetROIs(const std::vector<std::vector<cv::Point>>& polygons);
  uint64_t WantsFrame() const {
    return frame_ts_ms_.load(std::memory_order_acquire);
  }

  void Stop() {
    // Push an empty result to unblock FetchOneFrameResults
    algorithm_->PushResult({{}, 0}, std::numeric_limits<uint64_t>::max());
  }

 private:
  static uint64_t NextId();

  uint64_t id_{NextId()};
  std::atomic<uint64_t> frame_ts_ms_{0};
  Task task_;
  std::unique_ptr<Algorithm> algorithm_;
  float conf_th_;
  // Single ROI (used by most tasks)
  double roi_area_{};
  cv::Rect roi_bbox_{};
  std::vector<cv::Point> roi_points_{};

  // Multi-ROI (used by some tasks like kBunkOff)
  std::vector<std::vector<cv::Point>> roi_polygons_{};
  std::vector<double> roi_areas_{};
  std::vector<cv::Rect> roi_bboxes_{};
  DetectorPool::AddTaskFunc add_task_callback_;
};
}  // namespace avs_analyzer
#endif  // ANALYZER_ANALYZER_H
