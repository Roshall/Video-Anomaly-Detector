#include "algorithm.h"


namespace avs_analyzer {
Algorithm::~Algorithm() = default;

Algorithm::QueueValue Algorithm::FetchOneFrameResults() {
    QueueValue result;
    result_queue_.wait_dequeue(result);
    return result;
}

void Algorithm::PushResult(const YOLODetector::ResultType& results, uint64_t frame_ts) {
  // Convert YOLODetector::ResultType (vector + ts) into QueueValue (vector + frame_ts)
  QueueValue r{ results.first, frame_ts };
  result_queue_.enqueue(std::move(r));
}
}  // namespace avs_analyzer
