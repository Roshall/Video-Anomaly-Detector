#ifndef ANALYZER_WORKER_H
#define ANALYZER_WORKER_H

#include <atomic>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <string_view>
#include <vector>

#include "event.h"
#include "frame_walk_time.h"
#include "jpeg_encoder.h"

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
  void AddMonitor(std::unique_ptr<Monitor> monitor);
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
  void OnStreamOpened(int video_index, int width, int height,
                                                 AVPixelFormat pix_fmt);
  [[nodiscard]] int VideoStreamIndex() const { return video_index_; }
  [[nodiscard]] int VideoWidth() const { return video_width_; }
  [[nodiscard]] int VideoHeight() const { return video_height_; }
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
       int current_highest_level{0};
  };

  using MonitorContextPtr = std::unique_ptr<MonitorContext>;

  void PrepareMonitorForStream(MonitorContext& ctx) const;
  void EnsureAllPipelinesReady(AVFrame* sample, int out_width, int out_height);
  MonitorContext* FindMonitorContext(const std::string& code) const;
  void StartPushThreadIfNeeded();
  void ClosePushThreadIfNeeded();

  std::string video_code_;
  int video_width_{0};
  int video_height_{0};
  int video_index_{-1};
  AVPixelFormat video_pix_fmt_{AV_PIX_FMT_NONE};
  std::string stream_url_{};
  std::string push_stream_url_{};
  FrameWallClock timer_;
  EventDetectorConfig event_cfg_;
  std::atomic<bool> state_{false};
  std::vector<std::unique_ptr<std::thread>> threads_;
  bool file_{false};
  std::vector<MonitorContextPtr> monitors_;
  mutable std::shared_mutex monitors_mutex_;

  bool GrabFrame(AVFrame* frame) const;
       void AlarmImage(const Monitor& monitor, const EventDetector& checker,
                                                               JPEGEncoder& pict_writer, std::string_view level,
                                                               bool push) noexcept;
  bool NeedPushStream() const;
};
}  // namespace avs_analyzer
#endif  // ANALYZER_WORKER_H
