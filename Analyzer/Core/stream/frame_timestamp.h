#ifndef ANALYZER_STREAM_FRAME_TIMESTAMP_H
#define ANALYZER_STREAM_FRAME_TIMESTAMP_H

#include <cstdint>
#include <string_view>

extern "C" {
#include <libavutil/rational.h>
#include <libavutil/frame.h>
}

namespace avs_analyzer {

// Detect-thread-only state for timestamp synthesis.
struct TimestampState {
  // Last resolved timestamp in milliseconds; -1 means "unknown", which makes
  // the next synthesised timestamp start at 0 (a new stream generation).
  int64_t last_ms{-1};
  // Set once the "missing PTS and frame rate" warning has been logged.
  bool warned{false};
};

// Resolves `frame`'s timestamp in `time_base`. When the frame carries no
// usable PTS, a timestamp is synthesised from `fps_hint` and `state`.
// Returns false when neither the PTS nor a frame rate is available; in that
// case the caller should drop the frame.
bool ResolveFrameTimestamp(AVFrame* frame, AVRational time_base,
                           AVRational fps_hint, int64_t& timestamp,
                           int64_t& timestamp_ms, TimestampState& state,
                           std::string_view log_context = {});

}  // namespace avs_analyzer

#endif  // ANALYZER_STREAM_FRAME_TIMESTAMP_H
