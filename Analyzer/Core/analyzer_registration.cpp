#include "analyzer_registration.h"

#include <utility>

namespace avs_analyzer {

AnalyzerRegistration::AnalyzerRegistration(
    unsigned long analyzer_id, DetectorPool::AddTaskFunc add_task,
    std::function<void()> on_destroy)
    : analyzer_id_(analyzer_id),
      add_task_(std::move(add_task)),
      on_destroy_(std::move(on_destroy)) {}

AnalyzerRegistration::~AnalyzerRegistration() {
  if (on_destroy_) {
    on_destroy_();
  }
}

void AnalyzerRegistration::AddFrame(
    uint64_t video_id, uint64_t frame_id, DetectorPool::ImagePtr image,
    std::shared_ptr<DetectorPool::SharedInference> shared) {
  if (add_task_) {
    add_task_(analyzer_id_, video_id, frame_id, std::move(image),
              std::move(shared));
  }
}

}  // namespace avs_analyzer
