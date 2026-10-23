#ifndef ANALYZER_ALARM_PIPELINE_H
#define ANALYZER_ALARM_PIPELINE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "alarm.h"
#include "combined_event_manager.h"
#include "event/event_pipeline.h"
#include "frame_walk_time.h"
#include "video_processing/jpeg_encoder.h"

namespace avs_analyzer {
class Scheduler;
struct Event;
struct AlarmJob;

// Owns the alarm side of one stream: HLS event writer, JPEG snapshots, immutable
// alarm jobs, and admin stream-state notifications. Blocking I/O is delegated
// to the Scheduler's alarm dispatcher.
class AlarmPipeline {
 public:
  AlarmPipeline(Scheduler* scheduler, std::string video_code, bool file,
                FrameWallClock& timer);
  ~AlarmPipeline();

  // Rebuilds the immutable settings and JPEG encoder for the given geometry.
  void Configure(int width, int height, int fps, int out_width, int out_height);

  // Closes any active HLS writer for `active_event_id`, then reconfigures.
  void OnGeometryChanged(int width, int height, int fps, int out_width,
                         int out_height, int64_t active_event_id);

  // Handles combined-event edge transitions (admin notification + HLS start).
  void OnTransition(CombinedEventManager::Result::Transition transition,
                    int64_t event_id, int64_t pts);

  // Encodes and submits an alarm image. Safe on the detect thread: the JPEG
  // bytes are produced before the job is enqueued.
  void EmitImage(const AlarmRequest& request);

  // Submits a flushed event segment (live or offline).
  void SubmitEvent(std::unique_ptr<Event> event, int64_t alarm_id,
                   int64_t start_pts);

  // Finalizes admin state and any open HLS writer when the stream ends.
  void FinalizeOnStreamEnd(bool had_active,
                           std::vector<std::unique_ptr<Event>> tail_events,
                           int64_t tail_id, bool canceled_all);

  // Marks the admin stream as idle (used when the stream is being removed).
  void DeactivateStream();

  void NotifyAdminStreamEvent(std::string_view state);
  [[nodiscard]] bool StreamActive() const {
    return stream_active_.load(std::memory_order_acquire);
  }
  [[nodiscard]] int Width() const { return width_; }
  [[nodiscard]] int Height() const { return height_; }
  [[nodiscard]] int Fps() const { return fps_; }

 private:
  void StartHlsWriter();
  void CloseHlsIntoJob(AlarmJob& job, int64_t id,
                       std::chrono::system_clock::time_point fallback_start);

  Scheduler* scheduler_{nullptr};
  std::string video_code_;
  bool file_{false};
  FrameWallClock& timer_;

  int width_{0};
  int height_{0};
  int fps_{25};
  int out_width_{0};
  int out_height_{0};

  std::shared_ptr<const Alarm> alarm_settings_;
  std::shared_ptr<JPEGEncoder> pict_writer_;

  std::unique_ptr<HlsEventWriter> hls_writer_;
  bool hls_playlist_reported_{false};
  std::chrono::system_clock::time_point hls_start_ts_{};
  std::atomic<bool> stream_active_{false};
  int64_t last_combined_start_pts_{0};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_ALARM_PIPELINE_H
