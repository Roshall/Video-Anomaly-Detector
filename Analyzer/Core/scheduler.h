#ifndef ANALYZER_SCHEDULER_H
#define ANALYZER_SCHEDULER_H

#include <concurrentqueue/moodycamel/concurrentqueue.h>  // lock-free queue

#include <memory>
#include <mutex>
#include <string>
#include <threadpool/BS_thread_pool.hpp>
#include <unordered_map>
#include <vector>

#include "analyzer.h"
#include "detector/constant_yolo.h"
#include "event.h"

using namespace moodycamel;

namespace avs_analyzer {
extern BS::thread_pool<0> g_th_pool;

struct Config;
class Worker;
class Algorithm;
struct Monitor;
struct Alarm;

class Scheduler {
 public:
  friend class Analyzer;

  explicit Scheduler(Config& config);
  ~Scheduler();
  Scheduler(const Scheduler&) = delete;
  Scheduler operator=(const Scheduler&) = delete;

  Config& GetConfig() const;
  void CleanUpLoop();
  void SetState(bool state);
  bool GetState() const;

  void AddAlarm(std::unique_ptr<Alarm> alarm);

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
  void RemoveWorker(const std::string& code);  // 加入到待实际删除队列
  void NotifyCleanUp();
  void ClearWorker();
  bool ContainsMonitor(const std::string& video_code, const std::string& code);
  DetectorPool::AddTaskFunc RegisterAnalyzerCallback(Analyzer& analyzer);

  void DeregisterAnalyzerCallback(Analyzer& analyzer);

 private:
  Config& config_;
  std::atomic<bool> state_{false};

  bool ContainsMonitor(const Monitor* monitor);
  size_t GetWorkerSize();
  void AddWorker(std::unique_ptr<Worker> worker);

  // each worker monitors one type of anomaly for a stream
  std::unordered_map<std::string, std::unique_ptr<Worker>>
      worker_map_;  // <video.code,Worker>
  std::mutex worker_map_mutex_;
  ConcurrentQueue<std::unique_ptr<Worker>> to_be_deleted_workers_queue_;


  ConcurrentQueue<std::unique_ptr<Alarm>> alarm_queue_;
  DetectorPool detector_pool_;

  std::counting_semaphore<> clean_up_cs_{0};

};
}  // namespace avs_analyzer
#endif  // ANALYZER_SCHEDULER_H
