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
#include "video_processing/video_context.h"

namespace avs_analyzer {
class Scheduler;
class StreamSession;
class EventPipeline;
class AlarmPipeline;
class EventRunner;
struct DecodedFrame;

// Composition root for one video stream. It owns the monitor registry, the
// stream session (opened in start()), and the event/alarm pipelines, and wires
// them together; the pipelines themselves know nothing about each other.
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
  void EnsurePipelines();
  void DetectEvent();
  // Per-frame handler called by StreamSession's decode loop. Returns false to
  // stop the loop.
  bool HandleFrame(EventRunner& runner, const DecodedFrame& decoded);
  void EnsureAnnotatorReady(bool force_rebuild = false);
  void RebuildEventManager();
  void SyncPushRequest();
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
  std::thread detect_thread_;
  FrameWallClock timer_;

  std::atomic<bool> state_{false};
  std::atomic<bool> canceled_all_monitors_{false};
  // start() may be called once; concurrent adders wait on this future.
  std::promise<bool> start_promise_;
  std::shared_future<bool> started_{start_promise_.get_future().share()};
  std::atomic<bool> start_settled_{false};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_WORKER_H
