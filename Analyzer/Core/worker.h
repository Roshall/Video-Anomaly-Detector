#ifndef ANALYZER_WORKER_H
#define ANALYZER_WORKER_H

#include <opencv2/core/hal/interface.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <string_view>
#include <vector>

#include "format_convertor.h"
#include "video_processing/dynamic_annotator.h"
#include "combined_event_manager.h"
#include "event.h"
#include "frame_walk_time.h"
#include "jpeg_encoder.h"
#include "metrics.h"
#include "monitor.h"
#include "monitor/monitor_registry.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace avs_analyzer {
class Scheduler;
class AvPullStream;
class AvPushStream;
class Analyzer;
struct Monitor;
struct Frame;
struct StreamResources;
using StreamResourcesPtr = std::shared_ptr<StreamResources>;

// Result of one decode attempt.
enum class GrabResult {
  kOk,
  kAgain,         // Retryable decoder state; back off before the next attempt.
  kReconnecting,  // Woken without data while a reconnect is in progress.
  kEof,
  kDecodeError,
  kFatal,
};

class Worker {
 public:
  Worker(Scheduler* scheduler, std::unique_ptr<Monitor> monitor,
         const EventDetectorConfig& event_cfg, bool file);
  ~Worker();
  void AddMonitor(std::unique_ptr<Monitor> monitor, const EventDetectorConfig& event_cfg);
  void AddMonitorsBatch(std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>>&& monitors);
  [[nodiscard]] size_t num_task() const;
  void GetAllMonitor(std::vector<Monitor>& monitors) const;
  void RemoveMonitor(const std::string& code);
  void RemoveMonitorsBatch(const std::vector<std::string>& codes);
  Worker(const Worker& other) = delete;
  Worker operator= (const Worker& other) = delete;

  void DetectEvent();

  bool start(std::string& msg);
  // Blocks until start() has settled its result (used by concurrent batch
  // adds that join a worker another request is still starting).
  bool WaitStarted(std::chrono::milliseconds timeout) const;

  [[nodiscard]] bool State() const;
  void Remove();
  void Stop();
  void OfflineAnalyze();
  bool ContainsMonitor(const std::string& code) const;
  [[nodiscard]] const std::string& Code() const;
  [[nodiscard]] std::string_view StreamUrl() const { return stream_url_; }
  void OnStreamOpened(VideoContext context);
  [[nodiscard]] int VideoStreamIndex() const { return video_index_.load(std::memory_order_acquire); }
  [[nodiscard]] int VideoWidth() const { return video_width_.load(std::memory_order_acquire); }
  [[nodiscard]] int VideoHeight() const { return video_height_.load(std::memory_order_acquire); }
  [[nodiscard]] std::string_view PushStreamUrl() const {
    return push_stream_url_;
  }
  [[nodiscard]] int PushQueueMaxFrames() const;
  void RecordPushFrameDrop() const;
  void CleanUpPushThread(bool detach=false);

  // Notify admin stream start/end state transitions
  void NotifyAdminStreamEvent(std::string_view state);
  std::atomic<bool> admin_stream_active_notified_{false};
  std::atomic<bool> canceled_all_monitors_{false};

  Scheduler* scheduler;
  std::unique_ptr<AvPullStream> in_stream{};
  // Push stream is accessed concurrently by decode thread, encoder thread,
  // and admin/API threads. Use atomic shared_ptr load/store for safe lifetime.
  std::shared_ptr<AvPushStream> out_stream{};

  friend class AvPullStream;
 private:
  using MonitorContextPtr = MonitorRegistry::Ptr;

  // Snapshot of monitor state for event processing (thread-safe, no raw
  // pointers; `ctx` only addresses the context's atomic level counter).
  struct MonitorEventSnapshot {
    MonitorContextPtr ctx;
    std::string monitor_code;
    EventDetector::State state;
    int64_t start_pts;
    int64_t timestamp;
    int64_t num_detection;
    int64_t event_duration;
    std::pair<std::vector<int>, std::vector<std::string>> levels;
    LevelMode level_mode;
    int current_highest_level;
    int64_t event_id{};
  };

  void PrepareMonitorForStream(MonitorContext& ctx) const;
  void EnsureAllPipelinesReady();
  void StartPushThreadIfNeeded();
  void ClosePushThreadIfNeeded();
  void RebuildEventManager();
  void EnsureAnnotatorReady(bool force_rebuild = false);
  [[nodiscard]] size_t EventBufferBudgetBytes() const;
  // Convertor is rebuilt on reconnect and published atomically; frames whose
  // geometry does not match the current convertor are skipped.
  struct ConvertorState {
    std::shared_ptr<FormatConvertor> convertor;
    int width{0};
    int height{0};
    AVPixelFormat format{AV_PIX_FMT_NONE};
  };
  void AddTasks(AVFrame* frame, AVRational time_base,
                AVRational fps_hint = AVRational{0, 1}) const;
  // Resolves the frame timestamp, synthesising one for missing PTS. Returns
  // false when the timestamp cannot be determined (unknown frame rate).
  bool ResolveFrameTimestamps(AVFrame* frame, AVRational time_base,
                              AVRational fps_hint, int64_t& timestamp,
                              int64_t& timestamp_ms) const;
  std::vector<std::pair<Task, std::u8string>> GetMonitorTasks() const;
  CombinedEventConfig ComputeCombinedEventConfig() const;
  CombinedEventConfig ComputeCombinedEventConfig(bool offline_mode) const;
  static int ComputeLevelIndex(
      const std::pair<std::vector<int>, std::vector<std::string>>& levels,
      LevelMode level_mode, int64_t num_detection, int64_t event_duration,
      std::string_view monitor_code);
  static uint64_t NextId();

  GrabResult GrabFrame(AVFrame* frame, StreamResourcesPtr& resources);
  void AlarmImage(const std::string& monitor_code, int64_t start_pts,
                  int64_t event_pts, std::shared_ptr<const Frame> frame,
                  std::shared_ptr<JPEGEncoder> pict_writer,
                  std::string_view level,
                  int64_t alarm_id) noexcept;
  bool NeedPushStream() const;
  
  std::string video_code_;
  VideoContext video_context_{.format= AV_PIX_FMT_NONE, .width=0, .height=0, .video_index=-1};
  // Fast-path atomics for hot getters used by pull/push stream loops.
  std::atomic<int> video_index_{-1};
  std::atomic<int> video_width_{0};
  std::atomic<int> video_height_{0};

  // Protects composite updates/reads of video_context_.
  mutable std::shared_mutex video_context_mutex_;
  std::string stream_url_{};
  std::string push_stream_url_{};
  FrameWallClock timer_;
  std::atomic<bool> state_{false};
  std::vector<std::unique_ptr<std::thread>> threads_;
  mutable std::mutex threads_mutex_;
  bool file_{false};
  mutable MonitorRegistry monitors_;
  // Last successfully resolved frame timestamp (ms) used to synthesise PTS
  // for frames that carry AV_NOPTS_VALUE.
  mutable std::atomic<int64_t> last_frame_ts_ms_{-1};
  mutable std::atomic<bool> nopts_logged_{false};
  mutable std::mutex push_stream_mutex_;
  // Swapped atomically so the detect thread never blocks on a rebuild.
  std::shared_ptr<CombinedEventManager> event_manager_;
  std::shared_ptr<DynamicAnnotator> annotator_;
  mutable std::mutex annotator_mutex_;
  uint64_t id_{NextId()};
  std::shared_ptr<const ConvertorState> convertor_state_;
  // Detect thread only: generation whose decoder buffers are current.
  uint64_t decoded_generation_{0};
  std::shared_ptr<WorkerMetrics> metrics_;
  // start() may be called once; concurrent adders wait on this future.
  std::promise<bool> start_promise_;
  std::shared_future<bool> started_{start_promise_.get_future().share()};
  std::atomic<bool> start_settled_{false};
};
}  // namespace avs_analyzer
#endif  // ANALYZER_WORKER_H
