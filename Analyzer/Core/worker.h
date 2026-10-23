#ifndef ANALYZER_WORKER_H
#define ANALYZER_WORKER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "event.h"
#include "frame_walk_time.h"
#include "metrics.h"
#include "monitor.h"
#include "monitor/monitor_registry.h"
#include "task.h"
#include "video_processing/video_context.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {
class Scheduler;
class StreamSession;
class EventPipeline;
class AlarmPipeline;
struct ConvertorState;

// Composition root for one video stream. It owns the monitor registry and the
// three pipelines (stream, event, alarm) and wires them together; the pipelines
// themselves know nothing about each other.
class Worker {
 public:
  Worker(Scheduler* scheduler, std::unique_ptr<Monitor> monitor,
         const EventDetectorConfig& event_cfg, bool file);
  ~Worker();
  Worker(const Worker& other) = delete;
  Worker operator=(const Worker& other) = delete;

  void AddMonitor(std::unique_ptr<Monitor> monitor,
                  const EventDetectorConfig& event_cfg);
  void AddMonitorsBatch(
      std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>>&&
          monitors);
  [[nodiscard]] size_t num_task() const;
  void GetAllMonitor(std::vector<Monitor>& monitors) const;
  void RemoveMonitor(const std::string& code);
  void RemoveMonitorsBatch(const std::vector<std::string>& codes);

  bool start(std::string& msg);
  // Blocks until start() has settled its result (used by concurrent batch
  // adds that join a worker another request is still starting).
  bool WaitStarted(std::chrono::milliseconds timeout) const;

  [[nodiscard]] bool State() const;
  void Remove();
  void Stop();
  void OfflineAnalyze();
  bool ContainsMonitor(const std::string& code) const;
  [[nodiscard]] const std::string& Code() const { return video_code_; }

 private:
  using MonitorContextPtr = MonitorRegistry::Ptr;

  void DetectEvent();
  void PrepareMonitorForStream(MonitorContext& ctx,
                               const VideoContext& video_context) const;
  void EnsureAnnotatorReady(bool force_rebuild = false);
  void RebuildEventManager();
  void SyncPushRequest();
  void WakeAnalyzers(const std::vector<MonitorContextPtr>& contexts) const;
  void StopAnalyzers(const std::vector<MonitorContextPtr>& contexts) const;
  void AddTasks(AVFrame* frame, AVRational time_base,
                AVRational fps_hint = AVRational{0, 1}) const;
  // Resolves the frame timestamp, synthesising one for missing PTS. Returns
  // false when the timestamp cannot be determined (unknown frame rate).
  bool ResolveFrameTimestamps(AVFrame* frame, AVRational time_base,
                              AVRational fps_hint, int64_t& timestamp,
                              int64_t& timestamp_ms) const;
  std::vector<std::pair<Task, std::u8string>> GetMonitorTasks() const;
  [[nodiscard]] bool NeedPushStream() const;
  void OnStreamOpened(const VideoContext& context);
  static uint64_t NextId();

  Scheduler* scheduler_{nullptr};
  uint64_t id_{NextId()};
  std::string video_code_;
  std::string stream_url_;
  std::string push_stream_url_;
  bool file_{false};
  mutable MonitorRegistry monitors_;
  std::shared_ptr<WorkerMetrics> metrics_;

  // Declared before the pipelines so they are destroyed last.
  std::unique_ptr<StreamSession> stream_;
  std::unique_ptr<EventPipeline> events_;
  std::unique_ptr<AlarmPipeline> alarms_;
  // Published convertor used by offline analysis, where there is no
  // StreamSession to own it.
  std::shared_ptr<const ConvertorState> offline_convertor_;
  std::thread detect_thread_;
  FrameWallClock timer_;

  std::atomic<bool> state_{false};
  std::atomic<bool> canceled_all_monitors_{false};
  // Last successfully resolved frame timestamp (ms) used to synthesise PTS
  // for frames that carry AV_NOPTS_VALUE.
  mutable std::atomic<int64_t> last_frame_ts_ms_{-1};
  mutable std::atomic<bool> nopts_logged_{false};
  // start() may be called once; concurrent adders wait on this future.
  std::promise<bool> start_promise_;
  std::shared_future<bool> started_{start_promise_.get_future().share()};
  std::atomic<bool> start_settled_{false};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_WORKER_H
