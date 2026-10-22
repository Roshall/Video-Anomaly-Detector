//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_DETECTION_ALGORITHM_H
#define ANALYZER_DETECTION_ALGORITHM_H
#include "algorithm.h"


namespace avs_analyzer {
class YOLODetector;
struct Config;
class DetectionAlgorithm: public Algorithm {
 public:
  DetectionAlgorithm() = default;
  bool TryProcessDetections(Detection& out,
                            std::chrono::milliseconds timeout) override;
};
}

#endif  // ANALYZER_DETECTION_ALGORITHM_H
