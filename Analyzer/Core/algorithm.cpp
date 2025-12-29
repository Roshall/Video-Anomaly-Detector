#include "algorithm.h"


namespace avs_analyzer {
Algorithm::~Algorithm() = default;

Algorithm::ResultType Algorithm::FetchOneFrameResults() {
    ResultType result;
    result_queue_.wait_dequeue(result);
    return result;
}

void Algorithm::PushResult(const ResultType& results, uint64_t frame_ts) {
  // Ensure the queued ResultType has the up-to-date frame timestamp
  ResultType r = results;
  r.second = static_cast<unsigned long>(frame_ts);
  result_queue_.enqueue(r);
}
}  // namespace avs_analyzer
