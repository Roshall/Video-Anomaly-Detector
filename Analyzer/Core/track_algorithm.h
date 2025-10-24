//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_TRACK_ALGORITHM_H
#define ANALYZER_TRACK_ALGORITHM_H

#include <opencv2/opencv.hpp>

#include "algorithm.h"
#include "detector/constant_yolo.h"
#include "tracker/ByteTrack/BYTETracker.h"

namespace avs_analyzer {
struct Config;
class TrackAlgorithm: public Algorithm {
 public:
  explicit TrackAlgorithm(const Config& config);
  Detection DetectObjects(const cv::Mat& image) override;

 private:
  int min_frame_stride_{30};
  byte_track::BYTETracker tracker_;
  std::shared_ptr<const YOLODetector> detector_;
};
}  // namespace avs_analyzer

#endif  // ANALYZER_TRACK_ALGORITHM_H
