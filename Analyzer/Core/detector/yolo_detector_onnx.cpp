//
// Created by lg on 10/11/25.
//
#include "yolo_detector_onnx.h"
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include <Utils/log.h>
namespace avs_analyzer {
namespace {
// How long a sink waits for another session to publish a shared result before
// falling back to running the model itself. Kept below the default analyzer
// result timeout (5s) so a waiting event detector is not starved.
constexpr auto kSharedResultWait = std::chrono::seconds(4);

std::string ModelKey(const DL_INIT_PARAM& params) {
  return params.modelPath + "#" + std::to_string(params.target_id);
}
}  // namespace

YOLODetector::YOLODetector(const DL_INIT_PARAM& params)
    : params_(params),
      model_(std::make_unique<YOLOv8Detect>()),
      metrics_(MetricsRegistry::Instance().GetOrCreateModel(ModelKey(params_))) {
  model_->CreateSession(params_);
}

YOLODetector::YOLODetector(std::string_view mode_path, bool use_accelerator,
                           int target_id)
    : params_(kDefaultParam),
      model_(std::make_unique<YOLOv8Detect>()) {
  params_.modelPath = mode_path;
  params_.target_id = target_id;
  params_.cudaEnable = use_accelerator;
  metrics_ = MetricsRegistry::Instance().GetOrCreateModel(ModelKey(params_));
  model_->CreateSession(params_);
}

YOLODetector::~YOLODetector() {
  Stop();
  std::lock_guard lock(lifecycle_mutex_);
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

void YOLODetector::AddTask(Task task) {
  const unsigned long analyzer_id = task.id[0];
  const auto outcome = mailboxes_.Push(std::move(task));
  if (outcome.result == AnalyzerMailboxes::PushResult::kRejected) {
    return;
  }
  if (outcome.buffered_delta != 0) {
    metrics_->queue_depth.fetch_add(outcome.buffered_delta,
                                    std::memory_order_relaxed);
  }
  if (outcome.result == AnalyzerMailboxes::PushResult::kSchedule) {
    ready_ids_.enqueue(analyzer_id);
  }
}

void YOLODetector::RegisterCallback(unsigned long id, Callback cb, size_t depth) {
  {
    std::unique_lock<std::shared_mutex> lock(callback_mutex_);
    callbacks_[id] = std::move(cb);
    metrics_->pending_analyzers.fetch_add(1, std::memory_order_relaxed);
  }
  mailboxes_.Add(id, depth);
  Start();
}

void YOLODetector::DeregisterCallback(unsigned long id) {
  {
    std::unique_lock<std::shared_mutex> lock(callback_mutex_);
    if (callbacks_.erase(id) > 0) {
      metrics_->pending_analyzers.fetch_sub(1, std::memory_order_relaxed);
    }
  }
  const size_t buffered = mailboxes_.Remove(id);
  if (buffered > 0) {
    metrics_->queue_depth.fetch_sub(buffered, std::memory_order_relaxed);
  }
  if (mailboxes_.Size() == 0) {
    Stop();
  }
}

void YOLODetector::Start() {
  std::lock_guard lock(lifecycle_mutex_);
  if (running_.load(std::memory_order_acquire)) {
    return;
  }
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  running_.store(true, std::memory_order_release);
  worker_thread_ = std::jthread([this] { Run(); });
}

void YOLODetector::Run() {
  while (running_.load(std::memory_order_acquire)) {
    unsigned long analyzer_id = 0;
    if (!ready_ids_.wait_dequeue_timed(analyzer_id,
                                       std::chrono::milliseconds(200))) {
      continue;
    }
    if (!running_.load(std::memory_order_acquire)) {
      break;
    }

    Task task;
    if (!mailboxes_.Acquire(analyzer_id, task)) {
      continue;
    }
    metrics_->queue_depth.fetch_sub(1, std::memory_order_relaxed);

    Callback cb;
    bool has_callback = false;
    {
      std::shared_lock<std::shared_mutex> lock(callback_mutex_);
      if (auto it = callbacks_.find(analyzer_id); it != callbacks_.end()) {
        cb = it->second;
        has_callback = true;
      }
    }

    std::vector<DL_RESULT> filtered;
    if (has_callback) {
      const uint64_t frame_ts = task.id[2];
      const uint64_t wanted_ts = cb.second();
      const bool stale = frame_ts < wanted_ts;
      if (!stale) {
        if (!task.image || task.image->empty()) {
          // Deliver an empty result so waiters are released.
        } else if (task.shared && !task.shared->TryClaim()) {
          // Another session is running this exact frame for the same task.
          // Reuse its result instead of duplicating the inference.
          if (!task.shared->AwaitResult(filtered, kSharedResultWait)) {
            LOGW("Shared inference result timed out; running inference locally");
            filtered = RunInference(*task.image);
          }
        } else {
          filtered = RunInference(*task.image);
          if (task.shared) {
            task.shared->Publish(filtered);
          }
        }
      }
      cb.first({filtered, frame_ts}, frame_ts);
    } else if (task.shared && task.shared->TryClaim()) {
      // This analyzer was removed after the frame was submitted, but other
      // sinks of the same task may still be waiting. Produce and publish the
      // shared result instead of letting them time out.
      if (task.image && !task.image->empty()) {
        filtered = RunInference(*task.image);
      }
      task.shared->Publish(std::move(filtered));
    }

    if (mailboxes_.Complete(analyzer_id)) {
      ready_ids_.enqueue(analyzer_id);
    }
  }
}

void YOLODetector::Stop() {
  running_.store(false, std::memory_order_release);
  // Wake the worker if it is blocked waiting for the next analyzer.
  ready_ids_.enqueue(0);
}

std::vector<DL_RESULT> YOLODetector::RunInference(const cv::Mat& image) {
  const auto infer_start = std::chrono::steady_clock::now();
  auto raw = model_->RunSession(image);
  const auto infer_elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - infer_start)
          .count();
  metrics_->RecordInference(static_cast<uint64_t>(infer_elapsed));
  return FilterResults(std::move(raw));
}

std::vector<DL_RESULT> YOLODetector::FilterResults(
    std::vector<DL_RESULT> input) const {
  std::vector<DL_RESULT> filtered;
  filtered.reserve(input.size());
  for (auto& entry : input) {
    if (entry.classId == params_.target_id) {
      filtered.push_back(entry);
    }
  }
  return filtered;
}
}  // namespace avs_analyzer
