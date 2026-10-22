#ifndef ANALYZER_METRICS_H
#define ANALYZER_METRICS_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace avs_analyzer {

// Per-worker counters. A worker owns one instance for its lifetime; hot paths
// increment atomics directly, so no registry lock is taken per frame.
struct WorkerMetrics {
  std::atomic<uint64_t> frames_decoded{0};
  std::atomic<uint64_t> frames_submitted{0};
  std::atomic<uint64_t> frames_skipped{0};
  std::atomic<uint64_t> frames_dropped{0};
  std::atomic<uint64_t> detect_wait_timeout{0};
  std::atomic<uint64_t> reconnects{0};
  std::atomic<uint64_t> alarm_jobs_enqueued{0};
  std::atomic<uint64_t> alarm_jobs_dropped{0};
  std::atomic<uint64_t> push_frames_dropped{0};

  [[nodiscard]] nlohmann::json ToJson() const {
    return {
        {"framesDecoded", frames_decoded.load(std::memory_order_relaxed)},
        {"framesSubmitted", frames_submitted.load(std::memory_order_relaxed)},
        {"framesSkipped", frames_skipped.load(std::memory_order_relaxed)},
        {"framesDropped", frames_dropped.load(std::memory_order_relaxed)},
        {"detectWaitTimeout", detect_wait_timeout.load(std::memory_order_relaxed)},
        {"reconnects", reconnects.load(std::memory_order_relaxed)},
        {"alarmJobsEnqueued", alarm_jobs_enqueued.load(std::memory_order_relaxed)},
        {"alarmJobsDropped", alarm_jobs_dropped.load(std::memory_order_relaxed)},
        {"pushFramesDropped", push_frames_dropped.load(std::memory_order_relaxed)},
    };
  }
};

// Per-model (model file + target id) counters shared by all inference sessions
// belonging to that model.
struct ModelMetrics {
  std::atomic<uint64_t> inferences_total{0};
  std::atomic<uint64_t> inference_ns_total{0};
  std::atomic<uint64_t> queue_depth{0};
  std::atomic<uint64_t> pending_analyzers{0};
  std::atomic<uint64_t> max_inference_ns{0};

  void RecordInference(uint64_t elapsed_ns) {
    inferences_total.fetch_add(1, std::memory_order_relaxed);
    inference_ns_total.fetch_add(elapsed_ns, std::memory_order_relaxed);
    uint64_t current = max_inference_ns.load(std::memory_order_relaxed);
    while (elapsed_ns > current &&
           !max_inference_ns.compare_exchange_weak(current, elapsed_ns,
                                                   std::memory_order_relaxed)) {
    }
  }

  [[nodiscard]] nlohmann::json ToJson() const {
    const auto count = inferences_total.load(std::memory_order_relaxed);
    const auto total_ns = inference_ns_total.load(std::memory_order_relaxed);
    return {
        {"inferencesTotal", count},
        {"inferenceNsTotal", total_ns},
        {"meanInferenceMs", count ? static_cast<double>(total_ns) / count / 1e6 : 0.0},
        {"maxInferenceMs",
         static_cast<double>(max_inference_ns.load(std::memory_order_relaxed)) / 1e6},
        {"queueDepth", queue_depth.load(std::memory_order_relaxed)},
        {"pendingAnalyzers", pending_analyzers.load(std::memory_order_relaxed)},
    };
  }
};

struct AlarmMetrics {
  std::atomic<uint64_t> pending_jobs{0};
  std::atomic<uint64_t> upload_failures{0};
  std::atomic<uint64_t> reencode_failures{0};

  [[nodiscard]] nlohmann::json ToJson() const {
    return {
        {"pendingJobs", pending_jobs.load(std::memory_order_relaxed)},
        {"uploadFailures", upload_failures.load(std::memory_order_relaxed)},
        {"reencodeFailures", reencode_failures.load(std::memory_order_relaxed)},
    };
  }
};

// Process-wide registry used by the read-only /api/metrics endpoint.
class MetricsRegistry {
 public:
  static MetricsRegistry& Instance() {
    static MetricsRegistry instance;
    return instance;
  }

  std::shared_ptr<WorkerMetrics> RegisterWorker(const std::string& code) {
    auto metrics = std::make_shared<WorkerMetrics>();
    std::lock_guard lock(mutex_);
    workers_[code] = metrics;
    return metrics;
  }

  void UnregisterWorker(const std::string& code) {
    std::lock_guard lock(mutex_);
    workers_.erase(code);
  }

  std::shared_ptr<ModelMetrics> GetOrCreateModel(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto& slot = models_[key];
    if (!slot) {
      slot = std::make_shared<ModelMetrics>();
    }
    return slot;
  }

  void RemoveModel(const std::string& key) {
    std::lock_guard lock(mutex_);
    models_.erase(key);
  }

  [[nodiscard]] nlohmann::json ToJson() const {
    nlohmann::json workers = nlohmann::json::object();
    nlohmann::json models = nlohmann::json::object();
    nlohmann::json alarm;
    {
      std::lock_guard lock(mutex_);
      for (const auto& [code, metrics] : workers_) {
        workers[code] = metrics->ToJson();
      }
      for (const auto& [key, metrics] : models_) {
        models[key] = metrics->ToJson();
      }
      alarm = alarm_.ToJson();
    }
    return {{"workers", std::move(workers)},
            {"models", std::move(models)},
            {"alarm", std::move(alarm)}};
  }

  AlarmMetrics& alarm() { return alarm_; }

 private:
  MetricsRegistry() = default;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<WorkerMetrics>> workers_;
  std::unordered_map<std::string, std::shared_ptr<ModelMetrics>> models_;
  AlarmMetrics alarm_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_METRICS_H
