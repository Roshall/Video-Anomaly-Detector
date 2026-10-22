#ifndef ANALYZER_ALGORITHM_H
#define ANALYZER_ALGORITHM_H

#include <chrono>

#include <opencv2/opencv.hpp>
#include <readerwriterqueue/readerwriterqueue.h>
#include <vector>
#include "detector/yolo_detector_onnx.h"

namespace avs_analyzer {
struct Detection {
  std::vector<cv::Rect> bbox;
  std::vector<float> confidences;
  uint64_t frame_ts{0};
};

class Algorithm {
 public:
  // Queue element: detection results + frame timestamp
  using QueueValue = std::pair<std::vector<DL_RESULT>, uint64_t>;
  Algorithm() = default;
  virtual ~Algorithm();

  // Waits up to `timeout` for the next inference result.
  // Returns false if no result arrived before the deadline.
  virtual bool TryProcessDetections(Detection& out,
                                    std::chrono::milliseconds timeout) = 0;
  void PushResult(const YOLODetector::ResultType& results, uint64_t frame_ts);

 protected:
  bool FetchOneFrameResults(QueueValue& out, std::chrono::milliseconds timeout);
  moodycamel::BlockingReaderWriterQueue<QueueValue> result_queue_;
};

}  // namespace avs_analyzer
#endif  // ANALYZER_ALGORITHM_H
