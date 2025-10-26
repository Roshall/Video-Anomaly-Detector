#ifndef ANALYZER_SCHEDULER_H
#define ANALYZER_SCHEDULER_H

#include <concurrentqueue/moodycamel/concurrentqueue.h>  // lock-free queue

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <threadpool/BS_thread_pool.hpp>
#include <unordered_map>
#include <vector>

#include "analyzer.h"
#include "event.h"

using namespace moodycamel;

namespace avs_analyzer {
extern BS::thread_pool<0> g_th_pool;

class Config;
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
  Monitor* GetMonitorByCode(const std::string& code);
  void MonitorAdd(std::unique_ptr<Monitor> monitor, int& result_code,
                  std::string& result_msg, const EventDetectorConfig& event_cfg,
                  float conf_th);
  void MonitorRemove(const std::string& code, int& result_code,
                     std::string& result_msg);
  // void StartAlarmThread();
  // ApiServer 对应的函数 end
  void RemoveWorker(const std::string& code);  // 加入到待实际删除队列
  void NotifyCleanUp();
  void ClearWorker();

 private:
  Config& config_;
  std::atomic<bool> state_{false};

  size_t GetWorkerSize();
  bool Contains(const std::string& code);
  bool AddWorker(std::unique_ptr<Worker> worker);
  Worker* GetWorkerByCode(const std::string& code);


  // each worker monitors one type of anomaly for a stream
  std::unordered_map<std::string, std::unique_ptr<Worker>>
      worker_map_;  // <monitor.code,Worker*>
  std::mutex worker_map_mutex_;
  ConcurrentQueue<std::unique_ptr<Worker>> to_be_deleted_workers_queue_;


  ConcurrentQueue<std::unique_ptr<Alarm>> alarm_queue_;

  std::counting_semaphore<> clean_up_cs_{0};

};
}  // namespace avs_analyzer
#endif  // ANALYZER_SCHEDULER_H
