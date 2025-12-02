#include "algorithm.h"


namespace avs_analyzer {
Algorithm::~Algorithm() = default;

Algorithm::ResultType Algorithm::FetchOneFrameResults() {
    ResultType result;
    result_queue_.wait_dequeue(result);
    return result;
}

void Algorithm::PushResult(const ResultType& results) {
  result_queue_.enqueue(results);
}
}  // namespace avs_analyzer
