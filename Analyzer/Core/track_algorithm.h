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
  explicit TrackAlgorithm(uint64_t min_frame_duration);
  bool TryProcessDetections(Detection& out,
                            std::chrono::milliseconds timeout) override;

 private:
  byte_track::BYTETracker tracker_;
  std::shared_ptr<const YOLODetector> detector_;
  uint64_t min_frame_duration_;
};
}  // namespace avs_analyzer

#endif  // ANALYZER_TRACK_ALGORITHM_H
