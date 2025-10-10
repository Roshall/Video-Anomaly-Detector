//
// Created by lg on 10/10/25.
//

#ifndef ANALYZER_FRAME_WALK_TIME_H
#define ANALYZER_FRAME_WALK_TIME_H
// FrameWallClock.hpp
#pragma once
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#include <chrono>
#include <optional>
#include <string>
#include <stdexcept>

class FrameWallClock {
 public:
  using sys_time = std::chrono::time_point<std::chrono::system_clock>;

  // Call after avformat_open_input()/avformat_find_stream_info()
  // and after you determine the video stream index.
  void init(AVFormatContext* fmt, int stream_index) {
    if (!fmt) throw std::invalid_argument("fmt == nullptr");
    if (stream_index < 0 || stream_index >= static_cast<int>(fmt->nb_streams))
      throw std::invalid_argument("bad stream_index");

    m_fmt = fmt;
    m_st  = fmt->streams[stream_index];
    m_tb  = m_st->time_base;

    // Prefer real epoch start if provided (microseconds since Unix epoch).
    if (fmt->start_time_realtime != AV_NOPTS_VALUE) {
      m_epoch_anchor = std::chrono::system_clock::time_point{
          std::chrono::microseconds(fmt->start_time_realtime)
      };
      m_pts_anchor = m_st->start_time != AV_NOPTS_VALUE
                         ? m_st->start_time
                         : int64_t{0};
      m_mode = Mode::RealtimeEpochProvided;
    } else {
      // Fallback: capture the first observed frame's PTS and wall clock
      // when compute() first sees a valid PTS.
      m_mode = Mode::ObservedAnchor;
    }
  }

  // Returns wall-clock time for this frame. If no PTS is available,
  // returns std::nullopt.
  std::optional<sys_time> compute(const AVFrame* frame) {
    if (!frame) return std::nullopt;
    int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                      ? frame->best_effort_timestamp
                      : frame->pts;

    if (pts == AV_NOPTS_VALUE || m_mode == Mode::ObservedAnchor) {
      return std::chrono::system_clock::now();
    }
    // Mode::RealtimeEpochProvided
    return m_epoch_anchor + pts_delta_duration(pts, m_pts_anchor);
  }

  // Helper to stringify as ISO-8601 (UTC) "YYYY-mm-ddTHH:MM:SS.mmmuuuZ"
  static std::string to_iso8601_utc(sys_time t) {
    using namespace std::chrono;
    auto us_since_epoch = duration_cast<microseconds>(t.time_since_epoch());
    auto s = duration_cast<seconds>(us_since_epoch);
    auto rem_us = us_since_epoch - s;

    std::time_t tt = s.count();
    std::tm gmt{};

    gmtime_r(&tt, &gmt);
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03d%03dZ",
                  gmt.tm_year + 1900, gmt.tm_mon + 1, gmt.tm_mday,
                  gmt.tm_hour, gmt.tm_min, gmt.tm_sec,
                  int((rem_us.count() / 1000) % 1000), int(rem_us.count() % 1000));
    return std::string(buf);
  }

 private:
  enum class Mode { RealtimeEpochProvided, ObservedAnchor };

  // Convert (pts - anchor) in stream TB to chrono duration
  std::chrono::microseconds pts_delta_duration(int64_t pts, int64_t anchor) const {
    int64_t delta = pts - anchor;
    // Convert stream time base -> AV_TIME_BASE (µs), then to chrono
    int64_t us = av_rescale_q(delta, m_tb, AV_TIME_BASE_Q);
    return std::chrono::microseconds{us};
  }

  AVFormatContext* m_fmt{nullptr};
  AVStream* m_st{nullptr};
  AVRational m_tb{0,1};
  Mode m_mode{Mode::ObservedAnchor};

  // Anchor PTS in stream time base
  int64_t m_pts_anchor;
  // Wall-clock anchor (system epoch)
  sys_time m_epoch_anchor;
};
#endif  // ANALYZER_FRAME_WALK_TIME_H
