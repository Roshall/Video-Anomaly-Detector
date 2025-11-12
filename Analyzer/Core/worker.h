#ifndef ANALYZER_WORKER_H
#define ANALYZER_WORKER_H

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <string_view>
#include <vector>

#include "annotator.h"
#include "combined_event_manager.h"
#include "event.h"
#include "frame_walk_time.h"
#include "jpeg_encoder.h"
#include "monitor.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace avs_analyzer {
class Scheduler;
class AvPullStream;
class AvPushStream;
class Analyzer;
struct Monitor;
struct Frame;
class Worker {
 public:
  Worker(Scheduler* scheduler, std::unique_ptr<Monitor> monitor,
         const EventDetectorConfig& event_cfg, bool file);
  ~Worker();
  void AddMonitor(std::unique_ptr<Monitor> monitor, const EventDetectorConfig& event_cfg);
  [[nodiscard]] size_t num_task() const;
  void GetAllMonitor(std::vector<Monitor>& monitors) const;
  void RemoveMonitor(const std::string& code);
  Worker(const Worker& other) = delete;
  Worker operator= (const Worker& other) = delete;

  // （线程）解码视频帧和实时分析视频帧
  void DecodeVideo();

  bool start(std::string& msg);

  [[nodiscard]] bool State() const;
  void Remove();
  void Stop();
  void OfflineAnalyze();
  bool ContainsMonitor(const std::string& code) const;
  [[nodiscard]] const std::string& Code() const;
  [[nodiscard]] std::string_view StreamUrl() const { return stream_url_; }
  void OnStreamOpened(VideoContext context);
  [[nodiscard]] int VideoStreamIndex() const { return video_context_.video_index; }
  [[nodiscard]] int VideoWidth() const { return video_context_.width; }
  [[nodiscard]] int VideoHeight() const { return video_context_.height; }
  [[nodiscard]] std::string_view PushStreamUrl() const {
    return push_stream_url_;
  }
  void CleanUpPushThread();

  Scheduler* scheduler;
  std::unique_ptr<AvPullStream> in_stream{};
  std::unique_ptr<AvPushStream> out_stream{};
 private:
  struct MonitorContext {
       std::unique_ptr<Monitor> monitor;
       std::unique_ptr<Analyzer> analyzer;
       std::unique_ptr<EventDetector> detector;
    std::unique_ptr<VideoOverlay> overlay;
       EventDetectorConfig event_cfg;
       int current_highest_level{0};
  };

  using MonitorContextPtr = std::unique_ptr<MonitorContext>;

  // Snapshot of monitor state for event processing (thread-safe, no pointers)
  struct MonitorEventSnapshot {
    std::string monitor_code;
    EventDetector::State state;
    int64_t start_pts;
    int64_t timestamp;
    int64_t num_detection;
    int64_t event_duration;
    std::pair<std::vector<int>, std::vector<std::string>> levels;
    LevelMode level_mode;
    int current_highest_level;
  };

  void PrepareMonitorForStream(MonitorContext& ctx) const;
  void EnsureAllPipelinesReady(int out_width, int out_height) const;
  void StartPushThreadIfNeeded();
  void ClosePushThreadIfNeeded();
  void RebuildEventManager();
  CombinedEventConfig ComputeCombinedEventConfig() const;
  int ComputeLevelIndex(
      const std::pair<std::vector<int>, std::vector<std::string>>& levels,
      LevelMode level_mode, int64_t num_detection, int64_t event_duration,
      std::string_view monitor_code) const;

  std::string video_code_;
  VideoContext video_context_{.format= AV_PIX_FMT_NONE, .width=0, .height=0, .video_index=-1};
  std::string stream_url_{};
  std::string push_stream_url_{};
  FrameWallClock timer_;
  std::atomic<bool> state_{false};
  std::vector<std::unique_ptr<std::thread>> threads_;
  bool file_{false};
  std::vector<MonitorContextPtr> monitors_;
  mutable std::shared_mutex monitors_mutex_;
  mutable std::mutex push_stream_mutex_;
  std::unique_ptr<CombinedEventManager> event_manager_;

  bool GrabFrame(AVFrame* frame) const;
  void AlarmImage(const std::string& monitor_code, int64_t start_pts,
                  int64_t event_pts, const Frame& frame,
                  JPEGEncoder& pict_writer, std::string_view level,
                  bool push) noexcept;
  bool NeedPushStream() const;
};
}  // namespace avs_analyzer
#endif  // ANALYZER_WORKER_H
