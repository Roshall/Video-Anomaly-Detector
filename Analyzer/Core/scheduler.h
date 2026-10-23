#ifndef ANALYZER_SCHEDULER_H
#define ANALYZER_SCHEDULER_H

#include <concurrentqueue/moodycamel/concurrentqueue.h>  // lock-free queue

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <threadpool/BS_thread_pool.hpp>
#include <unordered_map>
#include <vector>

#include "analyzer.h"
#include "detector/constant_yolo.h"
#include "event.h"
#include "video_processing/frame_recorder.h"

using namespace moodycamel;

class AlarmController;

namespace avs_analyzer {
// Thread pools split by purpose so a long offline analysis or a slow alarm
// upload can never starve latency-sensitive control work.
extern BS::thread_pool<0> g_control_pool;
extern BS::thread_pool<0> g_media_pool;
extern BS::thread_pool<0> g_alarm_pool;

struct Config;
class AlarmIoDispatcher;
class AnalyzerRegistration;
class Worker;
class Algorithm;
struct Monitor;
struct Alarm;

// Immutable description of one alarm output unit. Owned end-to-end by the
// queue; nothing in it is mutated after enqueue.
struct AlarmJob {
  std::shared_ptr<const Alarm> alarm;
  int64_t id{};
  std::unique_ptr<Event> event;
  std::chrono::time_point<std::chrono::system_clock> start_ts;
  std::string video_code;
  // Set once when the live playlist should be reported to admin.
  std::optional<Recorder::HlsPaths> hls_playlist;
  // Set on the final job of an HLS event; the cleanup thread reencodes it.
  std::optional<Recorder::HlsPaths> hls_paths;
  // Frames were already written to the detect-thread HLS writer, so only the
  // playlist report applies.
  bool captured_by_hls{false};
};

class Scheduler {
 public:
  explicit Scheduler(Config& config);
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler operator=(const Scheduler&) = delete;

  Config& GetConfig() const;
  void CleanUpLoop();
  void SetState(bool state);
  bool GetState() const;

  void AddAlarm(AlarmJob job);

  // Runs a best-effort alarm side effect (JPEG write + admin POST) on the
  // bounded alarm I/O queue. Returns false when the queue is full and the
  // task was dropped.
  bool SubmitAlarmIo(std::function<void()> task);

  // Stops the alarm I/O dispatcher after every queued job has been finalized.
  void DrainAlarms();

  AlarmController* GetAlarmController() noexcept { return alarm_controller_.get(); }
  const AlarmController* GetAlarmController() const noexcept { return alarm_controller_.get(); }

  // ApiServer 对应的函数 start
  std::vector<Monitor> GetAllMonitors();
  void MonitorAdd(std::unique_ptr<Monitor> monitor, int& result_code,
                  std::string& result_msg,
                  const EventDetectorConfig& event_cfg);
  void MonitorRemove(const std::string& video_code, const std::string& code,
                     int& result_code, std::string& result_msg);
  
  // Batch operations
  void MonitorBatchAdd(std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>>&& monitors,
                       std::vector<std::pair<int, std::string>>& results);
  void MonitorBatchRemove(const std::vector<std::pair<std::string, std::string>>& monitor_ids,
                          std::vector<std::pair<int, std::string>>& results);
  // void StartAlarmThread();
  // ApiServer 对应的函数 end
  // Removes the worker registered for `code`. When `expected` is non-null the
  // entry is only removed if it still belongs to that worker, so a dying
  // worker can never evict a replacement created for the same video code.
  void RemoveWorker(const std::string& code,
                    const Worker* expected = nullptr);  // 加入到待实际删除队列
  void NotifyCleanUp();
  void ClearWorker();
  bool ContainsMonitor(const std::string& video_code, const std::string& code);
  // Registers the analyzer's mailbox and returns an RAII handle. The handle
  // deregisters the mailbox when the monitor context is destroyed.
  std::shared_ptr<AnalyzerRegistration> CreateAnalyzerRegistration(
      Analyzer& analyzer, size_t depth = 1);

  // One entry per loaded model runtime: file, target class, session count,
  // running state. Used by the model-load test endpoint.
  [[nodiscard]] nlohmann::json GetModelStatus() const;

 private:
  Config& config_;
  std::atomic<bool> state_{false};

  std::unique_ptr<AlarmController> alarm_controller_;

  bool ContainsMonitor(const Monitor* monitor);
  size_t GetWorkerSize();
  void RemoveWorkerIf(const std::string& code,
                      const std::shared_ptr<Worker>& expected);
  // Finalizes queued alarms and releases queued workers. Loops until both
  // queues stay empty.
  void DrainQueues();

  // each worker monitors one type of anomaly for a stream
  std::unordered_map<std::string, std::shared_ptr<Worker>>
      worker_map_;  // <video.code,Worker>
  std::mutex worker_map_mutex_;
  // Serializes worker construction (which loads models and fonts) so it can
  // happen outside worker_map_mutex_ without creating duplicates.
  std::mutex worker_creation_mutex_;
  ConcurrentQueue<std::shared_ptr<Worker>> to_be_deleted_workers_queue_;

  std::unique_ptr<AlarmIoDispatcher> alarm_dispatcher_;
  DetectorPool detector_pool_;

  std::counting_semaphore<> clean_up_cs_{0};

};
}  // namespace avs_analyzer
#endif  // ANALYZER_SCHEDULER_H
