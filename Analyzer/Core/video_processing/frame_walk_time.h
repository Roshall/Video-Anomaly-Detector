//
// Created by lg on 10/10/25.
//

#ifndef ANALYZER_FRAME_WALK_TIME_H
#define ANALYZER_FRAME_WALK_TIME_H


extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#include <chrono>
#include <optional>
#include <string>
#include <stdexcept>

namespace avs_analyzer {
class FrameWallClock {
 public:
  using sys_time = std::chrono::time_point<std::chrono::system_clock>;


  void init(const AVFormatContext* fmt, const AVStream* stream) {
    if (!fmt) throw std::invalid_argument("fmt == nullptr");

    time_base = stream->time_base;
    // Prefer real epoch start if provided (microseconds since Unix epoch).
    if (fmt->start_time_realtime != AV_NOPTS_VALUE) {
      m_epoch_anchor = std::chrono::system_clock::time_point{
          std::chrono::microseconds(fmt->start_time_realtime)};
      m_pts_anchor =
          stream->start_time != AV_NOPTS_VALUE ? stream->start_time : int64_t{0};
      m_mode = Mode::RealtimeEpochProvided;
    } else {
      // Fallback: capture the first observed frame's PTS and wall clock
      // when compute() first sees a valid PTS.
      m_mode = Mode::ObservedAnchor;
    }
  }

  sys_time compute(int64_t pts) {
    if (pts == AV_NOPTS_VALUE || m_mode == Mode::ObservedAnchor) {
      return std::chrono::system_clock::now();
    }
    // Mode::RealtimeEpochProvided
    return m_epoch_anchor + pts_delta_duration(pts, m_pts_anchor);
  }

  // Helper to stringify as ISO-8601 (UTC) "YYYY-mm-ddTHH:MM:SS.mmmuuuZ"
  static std::string to_iso8601_utc(sys_time t) {
    using namespace std::chrono;
    t += hours(8); // (Beijing is UTC+8)
    auto us_since_epoch = duration_cast<microseconds>(t.time_since_epoch());
    auto s = duration_cast<seconds>(us_since_epoch);
    auto rem_us = us_since_epoch - s;

    std::time_t tt = s.count();
    std::tm gmt{};

    gmtime_r(&tt, &gmt);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                  gmt.tm_year + 1900, gmt.tm_mon + 1, gmt.tm_mday, gmt.tm_hour,
                  gmt.tm_min, gmt.tm_sec, int((rem_us.count() / 1000) % 1000));
    return {buf};
  }

  // Returns wall-clock time for this frame. If no PTS is available,
  // returns std::nullopt.
  sys_time compute(const AVFrame* frame) {
    if (!frame) throw std::invalid_argument("fmt == nullptr");
    int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                      ? frame->best_effort_timestamp
                      : frame->pts;

    return compute(pts);
  }

 private:
  enum class Mode { RealtimeEpochProvided, ObservedAnchor };

  // Convert (pts - anchor) in stream TB to chrono duration
  std::chrono::microseconds pts_delta_duration(int64_t pts,
                                               int64_t anchor) const {
    int64_t delta = pts - anchor;
    // Convert stream time base -> AV_TIME_BASE (µs), then to chrono
    int64_t us = av_rescale_q(delta, time_base, AV_TIME_BASE_Q);
    return std::chrono::microseconds{us};
  }

  Mode m_mode{Mode::ObservedAnchor};
  AVRational time_base{};
  // Anchor PTS in stream time base
  int64_t m_pts_anchor{};
  // Wall-clock anchor (system epoch)
  sys_time m_epoch_anchor{};
};
}
#endif  // ANALYZER_FRAME_WALK_TIME_H
