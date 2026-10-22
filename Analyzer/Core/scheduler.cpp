#include "scheduler.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <ranges>
#include <string>
#include <vector>

#include "Utils/log.h"
#include "alarm.h"
#include "alarm_sound.h"
#include "analyzer.h"
#include "config.h"
#include "detector/constant_yolo.h"
#include "metrics.h"
#include "monitor.h"
#include "worker.h"

namespace avs_analyzer {
BS::thread_pool<> g_th_pool{3};

Scheduler::Scheduler(Config& config)
    : config_(config), detector_pool_(config) {
  if (!config_.alarm_sound_path.empty()) {
    try {
      alarm_controller_ = std::make_unique<AlarmController>(
          config_.alarm_sound_path, config_.alarm_sound_cooldown_sec);
    } catch (const std::exception& e) {
      LOGE("failed to init AlarmController: {}", e.what());
      alarm_controller_.reset();
    }
  }
}

Scheduler::~Scheduler() {
  state_.store(false, std::memory_order_release);
  DrainQueues();
}

Config& Scheduler::GetConfig() const { return config_; }

void Scheduler::CleanUpLoop() {
  while (state_.load(std::memory_order_acquire)) {
    clean_up_cs_.try_acquire_for(std::chrono::milliseconds(100));
    DrainQueues();
  }
  DrainQueues();
}

void Scheduler::DrainQueues() {
  bool progress = true;
  while (progress) {
    progress = false;
    AlarmJob job;
    uint64_t finalized = 0;
    while (alarm_queue_.try_dequeue(job)) {
      if (job.alarm) {
        job.alarm->Finalize(std::move(job));
      }
      ++finalized;
      progress = true;
    }
    if (finalized) {
      MetricsRegistry::Instance().alarm().pending_jobs.fetch_sub(
          finalized, std::memory_order_relaxed);
    }
    // Dropping the last reference may join the worker's threads, which can
    // enqueue tail alarm jobs that the loop above will pick up next round.
    std::shared_ptr<Worker> worker;
    while (to_be_deleted_workers_queue_.try_dequeue(worker)) {
      worker.reset();
      progress = true;
    }
  }
}

std::vector<Monitor> Scheduler::GetAllMonitors() {
  std::vector<std::shared_ptr<Worker>> workers;
  {
    std::lock_guard lock(worker_map_mutex_);
    workers.reserve(worker_map_.size());
    for (const auto& worker : worker_map_ | std::views::values) {
      workers.push_back(worker);
    }
  }

  size_t monitor_count = 0;
  for (const auto& worker : workers) {
    if (worker) {
      monitor_count += worker->num_task();
    }
  }
  std::vector<Monitor> monitors;
  monitors.reserve(monitor_count);
  for (const auto& worker : workers) {
    if (worker) {
      worker->GetAllMonitor(monitors);
    }
  }
  return monitors;
}

void Scheduler::MonitorAdd(std::unique_ptr<Monitor> monitor, int& result_code,
                           std::string& result_msg,
                           const EventDetectorConfig& event_cfg) {
  std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>> monitors;
  monitors.emplace_back(std::move(monitor), event_cfg);
  
  std::vector<std::pair<int, std::string>> results;
  MonitorBatchAdd(std::move(monitors), results);
  
  if (!results.empty()) {
    result_code = results[0].first;
    result_msg = results[0].second;
  } else {
    result_code = 0;
    result_msg = "internal error";
  }
}
void Scheduler::MonitorRemove(const std::string& video_code,
                              const std::string& code, int& result_code,
                              std::string& result_msg) {
  std::vector<std::pair<std::string, std::string>> monitor_ids;
  monitor_ids.emplace_back(video_code, code);
  
  std::vector<std::pair<int, std::string>> results;
  MonitorBatchRemove(monitor_ids, results);
  
  if (!results.empty()) {
    result_code = results[0].first;
    result_msg = results[0].second;
  } else {
    result_code = 0;
    result_msg = "internal error";
  }
}

void Scheduler::MonitorBatchAdd(std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>>&& monitors,
                                std::vector<std::pair<int, std::string>>& results) {
  results.clear();
  results.resize(monitors.size(), {0, ""});

  if (monitors.empty()) {
    return;
  }

  if (!state_.load(std::memory_order_acquire)) {
    for (auto& r : results) {
      r = {0, "service is stopping"};
    }
    return;
  }

  const bool all_file = std::ranges::all_of(monitors, [](const auto& m) {
    return m.first && m.first->source_type == "file";
  });

  // Atomic semantics for file batches: all success or all fail.
  if (all_file) {
    std::string fail_msg;
    bool ok = true;

    for (size_t i = 0; i < monitors.size(); ++i) {
      const auto& monitor = monitors[i].first;
      if (!monitor) {
        ok = false;
        fail_msg = "invalid monitor";
        break;
      }
      if (monitor->video_code.empty()) {
        ok = false;
        fail_msg = "videoCode is empty";
        break;
      }
      if (ContainsMonitor(monitor.get())) {
        ok = false;
        fail_msg = "the control is running";
        break;
      }
    }

    if (!ok) {
      for (auto& r : results) {
        r = {0, fail_msg};
      }
      return;
    }

    for (size_t i = 0; i < monitors.size(); ++i) {
      auto& [monitor, event_cfg] = monitors[i];
      g_th_pool.detach_task(
          [worker = std::make_unique<Worker>(this, std::move(monitor), event_cfg, true)]() mutable {
            worker->OfflineAnalyze();
          });
      results[i] = {1000, "added successfully"};
    }
    return;
  }

  // Stream (or mixed) semantics: per-item results.
  // Group monitors by video_code and source_type for efficient batch processing
  std::unordered_map<std::string, std::vector<size_t>> stream_groups;
  std::vector<size_t> file_indices;

  for (size_t i = 0; i < monitors.size(); ++i) {
    const auto& monitor = monitors[i].first;
    if (!monitor) {
      results[i] = {0, "invalid monitor"};
      continue;
    }

    if (monitor->video_code.empty()) {
      results[i] = {0, "videoCode is empty"};
      monitors[i].first.reset();
      continue;
    }

    if (ContainsMonitor(monitor.get())) {
      // keep legacy code semantics: already running treated as ok
      results[i] = {1000, "the control is running"};
      monitors[i].first.reset();
      continue;
    }

    if (monitor->source_type == "file") {
      file_indices.push_back(i);
    } else {
      stream_groups[monitor->video_code].push_back(i);
    }
  }

  // Process file monitors (non-atomic in mixed mode)
  for (size_t idx : file_indices) {
    auto& [monitor, event_cfg] = monitors[idx];
    if (!monitor) continue;
    g_th_pool.detach_task(
        [worker = std::make_shared<Worker>(this, std::move(monitor), event_cfg, true)]() mutable {
          worker->OfflineAnalyze();
        });
    results[idx] = {1000, "added successfully"};
  }

  // Process stream monitors in batches per video_code
  for (auto& [video_code, indices] : stream_groups) {
    std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>> batch;
    std::vector<size_t> valid_indices;
    
    for (size_t idx : indices) {
      auto& [monitor, event_cfg] = monitors[idx];
      if (!monitor) continue; // Already processed
      
      batch.emplace_back(std::move(monitor), event_cfg);
      valid_indices.push_back(idx);
    }
    
    if (batch.empty()) continue;

    // Find or create the worker under the map lock; start() and monitor
    // registration run outside it. Concurrent adders that find a worker still
    // starting wait for its start result instead of creating a second one.
    bool handled = false;
    for (int attempt = 0; attempt < 2 && !handled; ++attempt) {
      std::shared_ptr<Worker> worker;
      bool created = false;
      {
        std::lock_guard lock(worker_map_mutex_);
        if (const auto it = worker_map_.find(video_code);
            it != worker_map_.end()) {
          worker = it->second;
        } else {
          worker = std::make_shared<Worker>(this, std::move(batch.front().first),
                                            batch.front().second, false);
          worker_map_.emplace(video_code, worker);
          created = true;
        }
      }
      if (!worker) {
        continue;
      }

      if (created) {
        std::string start_msg;
        if (!worker->start(start_msg)) {
          LOGW("failed to start worker {}: {}", video_code, start_msg);
          RemoveWorkerIf(video_code, worker);
          for (size_t idx : valid_indices) {
            results[idx] = {0, "Internal error: cannot start a worker"};
          }
          handled = true;
          break;
        }
        // batch[0] was consumed by the constructor; add the rest in one batch.
        if (batch.size() > 1) {
          std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>>
              remaining;
          remaining.reserve(batch.size() - 1);
          for (size_t i = 1; i < batch.size(); ++i) {
            remaining.push_back(std::move(batch[i]));
          }
          worker->AddMonitorsBatch(std::move(remaining));
        }
      } else {
        if (!worker->WaitStarted(std::chrono::seconds(10))) {
          // The creator failed or timed out; retry creating a new worker.
          continue;
        }
        worker->AddMonitorsBatch(std::move(batch));
      }
      for (size_t idx : valid_indices) {
        results[idx] = {1000, "added successfully"};
      }
      handled = true;
    }

    if (!handled) {
      for (size_t idx : valid_indices) {
        results[idx] = {0, "Internal error: cannot start a worker"};
      }
    }
  }
}

void Scheduler::MonitorBatchRemove(const std::vector<std::pair<std::string, std::string>>& monitor_ids,
                                   std::vector<std::pair<int, std::string>>& results) {
  results.clear();
  results.reserve(monitor_ids.size());
  
  // Group monitors by video_code for efficient batch removal
  std::unordered_map<std::string, std::vector<std::pair<size_t, std::string>>> grouped;
  for (size_t i = 0; i < monitor_ids.size(); ++i) {
    const auto& [video_code, code] = monitor_ids[i];
    grouped[video_code].emplace_back(i, code);
  }
  
  // Reserve result slots
  results.resize(monitor_ids.size(), {1000, ""});

  // Snapshot the workers under the lock; removal itself runs unlocked.
  std::unordered_map<std::string, std::shared_ptr<Worker>> workers;
  {
    std::lock_guard lock(worker_map_mutex_);
    for (const auto& [video_code, index_code_pairs] : grouped) {
      if (const auto it = worker_map_.find(video_code);
          it != worker_map_.end()) {
        workers.emplace(video_code, it->second);
      }
    }
  }

  for (const auto& [video_code, index_code_pairs] : grouped) {
    if (const auto it = workers.find(video_code); it != workers.end()) {
      // Collect codes for batch removal
      std::vector<std::string> codes;
      codes.reserve(index_code_pairs.size());
      for (const auto& [idx, code] : index_code_pairs) {
        codes.push_back(code);
      }

      // Batch remove from worker
      if (it->second) {
        it->second->RemoveMonitorsBatch(codes);
      }

      // Set all results for this group
      for (const auto& [idx, code] : index_code_pairs) {
        results[idx] = {1000, "removing control, removed successfully"};
      }
    } else {
      // Worker not found for this video_code
      for (const auto& [idx, code] : index_code_pairs) {
        results[idx] = {1000, "control does not exist, removed successfully"};
      }
    }
  }
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
void Scheduler::RemoveWorker(const std::string& code) {
  RemoveWorkerIf(code, nullptr);
}
void Scheduler::RemoveWorkerIf(const std::string& code,
                               const std::shared_ptr<Worker>& expected) {
  std::shared_ptr<Worker> worker_to_delete;
  {
    std::lock_guard lock(worker_map_mutex_);
    const auto it = worker_map_.find(code);
    if (it == worker_map_.end()) {
      return;
    }
    if (expected && it->second != expected) {
      return;
    }
    worker_to_delete = std::move(it->second);
    worker_map_.erase(it);
  }
  to_be_deleted_workers_queue_.enqueue(std::move(worker_to_delete));
  NotifyCleanUp();
}
void Scheduler::NotifyCleanUp() {
  clean_up_cs_.release();
}

void Scheduler::AddAlarm(AlarmJob job) {
  alarm_queue_.enqueue(std::move(job));
  MetricsRegistry::Instance().alarm().pending_jobs.fetch_add(
      1, std::memory_order_relaxed);
  NotifyCleanUp();
}
void Scheduler::ClearWorker() {
  std::vector<std::shared_ptr<Worker>> workers;
  {
    std::lock_guard lock(worker_map_mutex_);
    workers.reserve(worker_map_.size());
    for (auto& worker : worker_map_ | std::views::values) {
      workers.push_back(std::move(worker));
    }
    worker_map_.clear();
  }
  for (auto& worker : workers) {
    if (worker) {
      to_be_deleted_workers_queue_.enqueue(std::move(worker));
    }
  }
  NotifyCleanUp();
}
bool Scheduler::ContainsMonitor(const std::string& video_code,
                                const std::string& code) {
  std::shared_ptr<Worker> worker;
  {
    std::lock_guard lock(worker_map_mutex_);
    const auto it = worker_map_.find(video_code);
    if (worker_map_.end() == it) {
      return false;
    }
    worker = it->second;
  }
  return worker && worker->ContainsMonitor(code);
}

DetectorPool::AddTaskFunc Scheduler::RegisterAnalyzerCallback(Analyzer& analyzer,
                                                              size_t depth) {
  return detector_pool_.RegisterToDetector(analyzer.task(), analyzer.id(),
                                           analyzer.detector_callback(), depth);
}

nlohmann::json Scheduler::GetModelStatus() const {
  return detector_pool_.GetModelStatus();
}

void Scheduler::DeregisterAnalyzerCallback(Analyzer& analyzer) {
  detector_pool_.DeregisterToDetector(analyzer.task(), analyzer.id());
}
}  // namespace avs_analyzer
