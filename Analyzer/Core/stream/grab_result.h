#ifndef ANALYZER_STREAM_GRAB_RESULT_H
#define ANALYZER_STREAM_GRAB_RESULT_H

namespace avs_analyzer {

// Result of one decode attempt from StreamSession::GrabFrame.
enum class GrabResult {
  kOk,
  kAgain,         // Retryable decoder state; back off before the next attempt.
  kReconnecting,  // Woken without data while a reconnect is in progress.
  kEof,
  kDecodeError,
  kFatal,
};

}  // namespace avs_analyzer

#endif  // ANALYZER_STREAM_GRAB_RESULT_H
