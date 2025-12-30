#include "scheduler.h"

#include <algorithm>
#include <mutex>
#include <ranges>
#include <string>
#include <vector>

#include "alarm.h"
#include "analyzer.h"
#include "config.h"
#include "detector/constant_yolo.h"
#include "monitor.h"
#include "worker.h"

namespace avs_analyzer {
BS::thread_pool<> g_th_pool{3};

Scheduler::Scheduler(Config& config) : config_(config), detector_pool_(config.model_dir, config.use_accelerator) {}

Scheduler::~Scheduler() = default;

Config& Scheduler::GetConfig() const { return config_; }

void Scheduler::CleanUpLoop() {
  while (state_) {
    clean_up_cs_.acquire();
    {
      std::unique_ptr<Alarm> alarm;
      while (alarm_queue_.try_dequeue(alarm)) {
        alarm->Finalize(config_.upload_dir, config_.admin_host);
      }
    }
    std::unique_ptr<Worker> worker;
    while (to_be_deleted_workers_queue_.try_dequeue(worker)) {
    }
  }
}

std::vector<Monitor> Scheduler::GetAllMonitors() {
  std::vector<Monitor> monitors;
  std::lock_guard lock(worker_map_mutex_);
  const auto worker_view = worker_map_ | std::views::values;
  size_t monitor_count = 0;
  for (const auto& monitor : worker_view) {
    monitor_count += monitor->num_task();
  }
  monitors.reserve(monitor_count);
  for (const auto& val : worker_view) {
    val->GetAllMonitor(monitors);
  }
  return monitors;
}

void Scheduler::MonitorAdd(std::unique_ptr<Monitor> monitor, int& result_code,
                           std::string& result_msg,
                           const EventDetectorConfig& event_cfg) {
  if (ContainsMonitor(monitor.get())) {
    result_msg = "the control is running";
    result_code = 1000;
    return;
  }
  if (monitor->video_code.empty()) {
    result_msg = "videoCode is empty";
    result_code = 0;
    return;
  }
  auto file = monitor->source_type == "file";
  if (file) {
    g_th_pool.detach_task(
        [worker = std::make_unique<Worker>(this, std::move(monitor), event_cfg, file)]() mutable  { worker->OfflineAnalyze(); });
    result_msg = "added successfully";
    result_code = 1000;
  } else {
    {
      std::lock_guard lock(worker_map_mutex_);
      if (const auto it = worker_map_.find(monitor->video_code);
          it != worker_map_.end()) {
        Worker* existing_worker = it->second.get();
        if (existing_worker->ContainsMonitor(monitor->code)) {
          result_msg = "control is running";
          result_code = 1000;
        } else {
          existing_worker->AddMonitor(std::move(monitor), event_cfg);
          result_msg = "added successfully";
          result_code = 1000;
        }
        return;
      }
    }
    
    // No existing worker, create a new one
    if (auto worker = std::make_unique<Worker>(
                   this, std::move(monitor), event_cfg, file);
               worker->start(result_msg)) {
      AddWorker(std::move(worker));
      result_msg = "added successfully";
      result_code = 1000;
    } else {
      result_msg = "Internal error: cannot start a worker";
      result_code = 0;
    }
  }
}
void Scheduler::MonitorRemove(const std::string& video_code,
                              const std::string& code, int& result_code,
                              std::string& result_msg) {
  std::lock_guard lock(worker_map_mutex_);
  if (const auto f = worker_map_.find(video_code); worker_map_.end() != f) {
    result_msg = "removing control, ";
    f->second->RemoveMonitor(code);
  } else {
    result_msg = "control does not exist, ";
  }
  result_msg += "removed successfully";
  result_code = 0;
}
void Scheduler::SetState(const bool state) { state_ = state; }
bool Scheduler::GetState() const { return state_; }

size_t Scheduler::GetWorkerSize() {
  std::lock_guard lock(worker_map_mutex_);
  return worker_map_.size();
}
bool Scheduler::ContainsMonitor(const Monitor* monitor) {
  return ContainsMonitor(monitor->video_code, monitor->code);
}
void Scheduler::AddWorker(std::unique_ptr<Worker> worker) {
  std::lock_guard lock(worker_map_mutex_);
  auto code = worker->Code();
  worker_map_.emplace(code, std::move(worker));
}
void Scheduler::RemoveWorker(const std::string& code) {
  bool result = false;
  std::unique_ptr<Worker> worker_to_delete;
  {
    std::lock_guard lock(worker_map_mutex_);
    if (auto f = worker_map_.find(code); worker_map_.end() != f) {
      worker_to_delete = std::move(f->second);
      worker_map_.erase(f);
      result = true;
    }
  }
  if (result) {
    to_be_deleted_workers_queue_.enqueue(std::move(worker_to_delete));
    NotifyCleanUp();
  }
}
void Scheduler::NotifyCleanUp() {
  clean_up_cs_.release();
}

void Scheduler::AddAlarm(std::unique_ptr<Alarm> alarm) {
  std::unique_ptr<Alarm> old_alarm;
  alarm_queue_.enqueue(std::move(alarm));
  NotifyCleanUp();
}
void Scheduler::ClearWorker() {
  worker_map_.clear();
}
bool Scheduler::ContainsMonitor(const std::string& video_code,
                                const std::string& code) {
  std::lock_guard lock(worker_map_mutex_);
  const auto it = worker_map_.find(video_code);
  if (worker_map_.end() == it) {
    return false;
  }
  return it->second->ContainsMonitor(code);
}

DetectorPool::AddTaskFunc Scheduler::RegisterAnalyzerCallback(Analyzer& analyzer) {
  return detector_pool_.RegisterToDetector(analyzer.task(), analyzer.id(), analyzer.detector_callback());
}

void Scheduler::DeregisterAnalyzerCallback(Analyzer& analyzer) {
  detector_pool_.DeregisterToDetector(analyzer.task(), analyzer.id());
}
}  // namespace avs_analyzer
