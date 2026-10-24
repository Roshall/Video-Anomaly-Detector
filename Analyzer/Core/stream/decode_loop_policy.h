#ifndef ANALYZER_STREAM_DECODE_LOOP_POLICY_H
#define ANALYZER_STREAM_DECODE_LOOP_POLICY_H

#include "stream/grab_result.h"

namespace avs_analyzer {

// What the decode loop should do with one GrabResult. Pure logic, no I/O, so
// the reconnect/error-budget state machine is unit-testable.
class DecodeLoopPolicy {
 public:
  // Consecutive decode errors after which the worker is removed.
  static constexpr int kMaxConsecutiveErrors = 50;
  static constexpr int kMaxBackoffMs = 500;
  static constexpr int kBackoffStepMs = 10;
  // Wait applied while a reconnect is in progress.
  static constexpr int kReconnectWaitMs = 20;

  enum class Action {
    kProcess,        // A frame was decoded; hand it to the handler.
    kBackoff,        // Retryable error; sleep backoff_ms() first.
    kWaitReconnect,  // Reconnect in progress; sleep kReconnectWaitMs.
    kStop,           // EOF or fatal error; end the loop.
    kRemoveWorker,   // Error budget exhausted; remove the worker and stop.
  };

  // Feeds one decode attempt and returns the loop action.
  Action OnResult(GrabResult result);

  // Backoff for the current error streak; 0 unless the action is kBackoff.
  [[nodiscard]] int backoff_ms() const { return backoff_ms_; }
  [[nodiscard]] int consecutive_errors() const {
    return consecutive_errors_;
  }
  void Reset();

 private:
  int consecutive_errors_{0};
  int backoff_ms_{0};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_STREAM_DECODE_LOOP_POLICY_H
