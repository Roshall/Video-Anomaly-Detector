//
// Created by lg on 10/11/25.
//

#include "track_algorithm.h"

#include "ByteTrack/Object.h"

namespace avs_analyzer {

TrackAlgorithm::TrackAlgorithm(uint64_t min_frame_duration)
  : min_frame_duration_(min_frame_duration)
{
  
}

Detection TrackAlgorithm::ProcessDetections() {
  auto [res, frame_ms] = FetchOneFrameResults();
  std::vector<byte_track::Object> objects;
  objects.reserve(res.size());
  for (auto& obj : res) {
    objects.emplace_back(byte_track::Rect(static_cast<float>(obj.box.x),
                                          static_cast<float>(obj.box.y),
                                          static_cast<float>(obj.box.width),
                                          static_cast<float>(obj.box.height)),
                         obj.classId, obj.confidence);
  }
  auto tracks = tracker_.update(objects, frame_ms);

  std::vector<cv::Rect> bbox;
  std::vector<float> confs;
  for (auto& t : tracks) {
    if (t->getFrameDuration() >= min_frame_duration_) {
      auto rec_f = t->getRect();
      bbox.emplace_back(rec_f.x(), rec_f.y(), rec_f.width(), rec_f.height());
      confs.push_back(t->getScore());
    }
  }
  return {bbox, confs};
}
}  // namespace avs_analyzer