#include "scheduler.h"

#include <algorithm>
#include <mutex>
#include <ranges>
#include <string>
#include <vector>

#include "alarm.h"
#include "config.h"
#include "monitor.h"
#include "worker.h"

namespace avs_analyzer {
BS::thread_pool<> g_th_pool{3};

Scheduler::Scheduler(Config& config) : config_(config) {}

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
bool Scheduler::GetMonitorByCode(const std::string& video_code,
                                 const std::string& code, Monitor* monitor_p) {
  Worker* worker = nullptr;
  {
    std::lock_guard lock(worker_map_mutex_);
    if (const auto it = worker_map_.find(video_code);
        it != worker_map_.end()) {
      worker = it->second.get();
    }
  }

  if (!worker) {
    return false;
  }

  std::vector<Monitor> monitors;
  worker->GetAllMonitor(monitors);
  const auto mit = std::find_if(monitors.begin(), monitors.end(),
                                [&code](const Monitor& monitor) {
                                  return monitor.code == code;
                                });
  if (mit == monitors.end()) {
    return false;
  }
  *monitor_p = *mit;
  return true;
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
    monitor->video_code = monitor->code;
  }
  auto file = monitor->source_type == "file";
  if (file) {
    g_th_pool.detach_task(
        [worker = std::make_unique<Worker>(this, std::move(monitor), event_cfg, file)]() mutable  { worker->OfflineAnalyze(); });
    result_msg = "added successfully";
    result_code = 1000;
  } else {
    Worker* existing_worker = nullptr;
    {
      std::lock_guard lock(worker_map_mutex_);
      if (const auto it = worker_map_.find(monitor->video_code);
          it != worker_map_.end()) {
        existing_worker = it->second.get();
      }
    }

    if (existing_worker) {
      if (existing_worker->ContainsMonitor(monitor->code)) {
        result_msg = "control is running";
        result_code = 1000;
      } else {
        existing_worker->AddMonitor(std::move(monitor));
        result_msg = "added successfully";
        result_code = 1000;
      }
    } else if (auto worker = std::make_unique<Worker>(
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
  if (Worker* worker = GetWorkerByVideoCode(video_code)) {
    if (worker->ContainsMonitor(code)) {
      result_msg = "removing control, ";
      worker->RemoveMonitor(code);
    } else {
      result_msg = "control does not exist, ";
    }

    result_msg += "cancel success";
    result_code = 1000;
    return;
  }
  result_msg = "there is no such control";
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
Worker* Scheduler::GetWorkerByVideoCode(const std::string& video_code) {
  Worker* found_worker = nullptr;
  {
    std::lock_guard lock(worker_map_mutex_);
    if (const auto f = worker_map_.find(video_code); worker_map_.end() != f) {
      found_worker = f->second.get();
    }
  }
  return found_worker;
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
}  // namespace avs_analyzer
