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
};

class Algorithm {
 public:
  using ResultType = YOLODetector::ResultType;
  Algorithm() = default;
  virtual ~Algorithm();

  virtual Detection ProcessDetections() = 0;
  ResultType FetchOneFrameResults();
  void PushResult(const ResultType& results, uint64_t frame_ts);

 protected:
  moodycamel::BlockingReaderWriterQueue<ResultType> result_queue_;
};

}  // namespace avs_analyzer
#endif  // ANALYZER_ALGORITHM_H
