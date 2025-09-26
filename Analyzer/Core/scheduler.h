#ifndef ANALYZER_SCHEDULER_H
#define ANALYZER_SCHEDULER_H

#include <concurrentqueue/moodycamel/concurrentqueue.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "analyzer.h"

using namespace moodycamel;

namespace avs_analyzer {
class Config;
class Worker;
class Algorithm;
struct Monitor;
struct AlarmImage;
struct Alarm;

class Scheduler {
 public:
  friend class Analyzer;

  explicit Scheduler(Config* config);
  ~Scheduler();

  Config* GetConfig() const;
  bool InitAlgorithm();
  void CleanUpLoop();
  void SetState(bool state);
  bool GetState() const;

  void AddAlarm(std::unique_ptr<Alarm> alarm);

  // ApiServer 对应的函数 start
  std::vector<Monitor*> GetAllMonitors();
  Monitor* GetMonitorByCode(const std::string& code);
  void MonitorAdd(std::unique_ptr<Monitor> monitor, int& result_code, std::string& result_msg);
  void MonitorRemove(const std::string& code, int& result_code,
                     std::string& result_msg);
  // void StartAlarmThread();
  // ApiServer 对应的函数 end
  void RemoveWorker(const std::string& code);  // 加入到待实际删除队列

 private:
  Config* config_;
  bool state_;

  
  size_t GetWorkerSize();
  bool Contains(const std::string& code);
  bool AddWorker(std::unique_ptr<Worker> worker);
  Worker* GetWorkerByCode(const std::string& code);
  
  
  // each worker monitors one type of anomaly for a stream
  std::unordered_map<std::string, std::unique_ptr<Worker>>
      worker_map_;  // <monitor.code,Worker*>
  std::mutex worker_map_mutex_;
  ConcurrentQueue<std::unique_ptr<Worker>> to_be_deleted_workers_queue_;

  // 报警处理 start
  // std::unique_ptr<std::thread> alarm_thread_;
  // static void AlarmLoopThread(void* arg);
  [[noreturn]] void PullAndGenAlarm();
  ConcurrentQueue<std::unique_ptr<Alarm>> alarm_queue_;
  
  void NotifyCleanUp();
  std::mutex clean_up_mtx_;
  bool clean_up_ready_ = false;
  std::condition_variable clean_up_cv_;

  // 报警处理 end
  std::mutex algorithm_mutex_;
  std::unique_ptr<Algorithm> yolov8n_80_classes_{};
};
}  // namespace avs_analyzer
#endif  // ANALYZER_SCHEDULER_H
