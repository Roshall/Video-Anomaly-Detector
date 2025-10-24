//
// Created by lg on 10/11/25.
//

#include "track_algorithm.h"

#include "ByteTrack/Object.h"
#include "ByteTrack/Rect.h"
#include "config.h"
#include "detector/yolo_detector_onnx.h"
#include "task.h"

namespace avs_analyzer {
TrackAlgorithm::TrackAlgorithm(const Config& config, int frame_stride)
    : detector_(GetDetector(config.model_dir, Task::kWandering,
                            config.use_accelerator)), min_frame_stride_{frame_stride} {}

Detection TrackAlgorithm::DetectObjects(const cv::Mat& image) {
  auto res = detector_->Run(image);
  std::vector<byte_track::Object> objects;
  objects.reserve(res.size());
  for (auto& obj : res) {
    objects.emplace_back(byte_track::Rect(static_cast<float>(obj.box.x),
                                          static_cast<float>(obj.box.y),
                                          static_cast<float>(obj.box.width),
                                          static_cast<float>(obj.box.height)),
                         obj.classId, obj.confidence);
  }
  auto tracks = tracker_.update(objects);

  std::vector<cv::Rect> bbox;
  std::vector<float> confs;
  for (auto& t : tracks) {
    if (t->getFrameId() - t->getStartFrameId() >= min_frame_stride_) {
      auto rec_f = t->getRect();
      bbox.emplace_back(rec_f.x(), rec_f.y(), rec_f.width(), rec_f.height());
      confs.push_back(t->getScore());
    }
  }
  return {bbox, confs};
}
}  // namespace avs_analyzer