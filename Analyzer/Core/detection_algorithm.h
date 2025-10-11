//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_DETECTION_ALGORITHM_H
#define ANALYZER_DETECTION_ALGORITHM_H
#include "algorithm.h"
#include "task.h"


namespace avs_analyzer {
class YOLODetector;
class DetectionAlgorithm: public Algorithm {
 public:
  DetectionAlgorithm(Config* config, Task task);
  Detection DetectObjects(const cv::Mat &image) override;
 private:
  std::shared_ptr<const YOLODetector> detector_;
};
}

#endif  // ANALYZER_DETECTION_ALGORITHM_H
