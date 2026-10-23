#ifndef ANALYZER_ANALYZER_REGISTRATION_H
#define ANALYZER_ANALYZER_REGISTRATION_H

#include <cstdint>
#include <functional>
#include <memory>

#include "detector/constant_yolo.h"

namespace avs_analyzer {

// RAII handle for one analyzer's mailbox registration in the detector pool.
// The handle keeps the AddTask bridge alive for exactly as long as the analyzer
// owns it, and deregisters on destruction so callbacks can never outlive the
// analyzer.
class AnalyzerRegistration {
 public:
  AnalyzerRegistration(unsigned long analyzer_id,
                       DetectorPool::AddTaskFunc add_task,
                       std::function<void()> on_destroy);
  ~AnalyzerRegistration();
  AnalyzerRegistration(const AnalyzerRegistration&) = delete;
  AnalyzerRegistration& operator=(const AnalyzerRegistration&) = delete;

  void AddFrame(uint64_t video_id, uint64_t frame_id,
                DetectorPool::ImagePtr image,
                std::shared_ptr<DetectorPool::SharedInference> shared);

 private:
  unsigned long analyzer_id_{0};
  DetectorPool::AddTaskFunc add_task_;
  std::function<void()> on_destroy_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_ANALYZER_REGISTRATION_H
