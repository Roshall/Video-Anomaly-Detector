#include "stream/decode_loop_policy.h"

#include <algorithm>

namespace avs_analyzer {

void DecodeLoopPolicy::Reset() {
  consecutive_errors_ = 0;
  backoff_ms_ = 0;
}

DecodeLoopPolicy::Action DecodeLoopPolicy::OnResult(GrabResult result) {
  switch (result) {
    case GrabResult::kOk:
      Reset();
      return Action::kProcess;
    case GrabResult::kAgain:
    case GrabResult::kDecodeError: {
      ++consecutive_errors_;
      if (consecutive_errors_ >= kMaxConsecutiveErrors) {
        return Action::kRemoveWorker;
      }
      backoff_ms_ = std::min(kMaxBackoffMs,
                             kBackoffStepMs * consecutive_errors_);
      return Action::kBackoff;
    }
    case GrabResult::kReconnecting:
      return Action::kWaitReconnect;
    case GrabResult::kEof:
    case GrabResult::kFatal:
      return Action::kStop;
  }
  return Action::kStop;
}

}  // namespace avs_analyzer
