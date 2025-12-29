#ifndef ANALYZER_ALGORITHM_H
#define ANALYZER_ALGORITHM_H

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

  virtual Detection ProcessDetections() = 0;
  QueueValue FetchOneFrameResults();
  void PushResult(const YOLODetector::ResultType& results, uint64_t frame_ts);

 protected:
  moodycamel::BlockingReaderWriterQueue<QueueValue> result_queue_;
};

}  // namespace avs_analyzer
#endif  // ANALYZER_ALGORITHM_H
