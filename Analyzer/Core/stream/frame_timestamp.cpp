#include "stream/frame_timestamp.h"

#include <cmath>

#include "Utils/log.h"

namespace avs_analyzer {

bool ResolveFrameTimestamp(AVFrame* frame, AVRational time_base,
                           AVRational fps_hint, int64_t& timestamp,
                           int64_t& timestamp_ms, TimestampState& state,
                           std::string_view log_context) {
  timestamp = frame->best_effort_timestamp;
  if (timestamp == AV_NOPTS_VALUE) {
    timestamp = frame->pts;
  }
  if (timestamp != AV_NOPTS_VALUE) {
    timestamp_ms = av_rescale_q(timestamp, time_base, AVRational{1, 1000});
    state.last_ms = timestamp_ms;
    return true;
  }

  int64_t step_ms = 0;
  if (fps_hint.num > 0 && fps_hint.den > 0) {
    step_ms = std::llround(1000.0 * fps_hint.den / fps_hint.num);
  }
  if (step_ms <= 0) {
    if (!state.warned) {
      state.warned = true;
      if (log_context.empty()) {
        LOGW("cannot resolve frame timestamps (missing PTS and frame rate); "
             "dropping frames");
      } else {
        LOGW("worker {} cannot resolve frame timestamps (missing PTS and "
             "frame rate); dropping frames",
             log_context);
      }
    }
    return false;
  }
  timestamp_ms = state.last_ms >= 0 ? state.last_ms + step_ms : 0;
  timestamp = av_rescale_q(timestamp_ms, AVRational{1, 1000}, time_base);
  // Keep the detect loop aligned with the synthesised timestamp.
  frame->best_effort_timestamp = timestamp;
  state.last_ms = timestamp_ms;
  return true;
}

}  // namespace avs_analyzer
