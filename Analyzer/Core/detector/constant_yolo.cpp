//
// Created by lg on 10/11/25.
//

#include "constant_yolo.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

#include "config.h"
#include "yolo_detector_onnx.h"
#include "Utils/log.h"

namespace avs_analyzer {
namespace {
struct ModelSpec {
  std::string file;
  int target_id{0};
};

std::optional<ModelSpec> ModelForTask(Task task) {
  switch (task) {
    case Task::kBreakIn:
    case Task::kWandering:
    case Task::kBunkOff:
      return ModelSpec{"yolov8n.onnx", 0};
    case Task::kSleepWork:
      return ModelSpec{"sleeping_m.onnx", 0};
    case Task::kWallClimbing:
      return ModelSpec{"climbing_m.onnx", 1};
    case Task::kPhonePlaying:
      return ModelSpec{"phone_playing_m.onnx", 1};
    default:
      return std::nullopt;
  }
}

int TargetIdForModelFile(const std::string& file) {
  if (file == "climbing_m.onnx" || file == "phone_playing_m.onnx") {
    return 1;
  }
  return 0;
}
}  // namespace

ModelRuntime::ModelRuntime(std::string model_file, bool use_accelerator,
                           int target_id, int session_count,
                           std::chrono::milliseconds shared_result_wait)
    : model_file_(std::move(model_file)), target_id_(target_id),
      shared_result_wait_(shared_result_wait) {
  DL_INIT_PARAM params{};
  params.modelPath = model_file_;
  params.target_id = target_id_;
  params.cudaEnable = use_accelerator;

  sessions_.reserve(static_cast<size_t>(std::max(1, session_count)));
  for (int i = 0; i < std::max(1, session_count); ++i) {
    const auto start = std::chrono::steady_clock::now();
    try {
      sessions_.push_back(
          std::make_unique<YOLODetector>(params, shared_result_wait));
    } catch (const std::exception& e) {
      if (!params.cudaEnable) {
        throw;
      }
      LOGW("Model {}: CUDA session failed ({}); falling back to CPU",
           model_file_, e.what());
      params.cudaEnable = false;
      sessions_.push_back(
          std::make_unique<YOLODetector>(params, shared_result_wait));
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    LOGI("Loaded model {} target {} session {}/{} in {} ms", model_file_,
         target_id_, i + 1, session_count, elapsed_ms);
  }
}

std::shared_ptr<YOLODetector> ModelRuntime::Register(unsigned long analyzer_id,
                                                     YOLODetector::Callback cb,
                                                     size_t depth) {
  std::lock_guard lock(registration_mutex_);
  const size_t index = next_session_.fetch_add(1) % sessions_.size();
  sessions_[index]->RegisterCallback(analyzer_id, std::move(cb), depth);
  assignments_[analyzer_id] = index;
  return sessions_[index];
}

bool ModelRuntime::Deregister(unsigned long analyzer_id) {
  size_t index = 0;
  {
    std::lock_guard lock(registration_mutex_);
    auto it = assignments_.find(analyzer_id);
    if (it == assignments_.end()) {
      return false;
    }
    index = it->second;
    assignments_.erase(it);
  }
  sessions_[index]->DeregisterCallback(analyzer_id);
  return true;
}

void ModelRuntime::StopAll() {
  for (auto& session : sessions_) {
    session->Stop();
  }
}

bool ModelRuntime::isRunning() const {
  for (const auto& session : sessions_) {
    if (session->isRunning()) {
      return true;
    }
  }
  return false;
}

DetectorPool::DetectorPool(const Config& config)
    : model_path_(config.model_dir),
      use_accelerator_(config.use_accelerator),
      workers_per_model_(config.inference_workers_per_model),
      workers_per_model_accelerated_(
          config.inference_workers_per_model_accelerated),
      shared_result_wait_(SharedResultWaitFor(
          std::chrono::milliseconds(config.inference_result_timeout_ms))) {
  const int sessions = use_accelerator_ ? workers_per_model_accelerated_
                                        : workers_per_model_;
  LOGI("DetectorPool: accelerator={} sessions/model={}", use_accelerator_,
       sessions);
  for (const auto& file : config.preload_models) {
    try {
      GetOrCreateRuntime(file, TargetIdForModelFile(file));
    } catch (const std::exception& e) {
      LOGE("Failed to preload model {}: {}", file, e.what());
    }
  }
}

DetectorPool::~DetectorPool() { StopAllDetectors(); }

std::string DetectorPool::RuntimeKey(const std::string& file, int target_id) {
  return file + "#" + std::to_string(target_id);
}

std::shared_ptr<ModelRuntime> DetectorPool::GetOrCreateRuntime(
    const std::string& file, int target_id) {
  const std::string key = RuntimeKey(file, target_id);
  std::shared_ptr<std::promise<std::shared_ptr<ModelRuntime>>> loader;
  std::shared_future<std::shared_ptr<ModelRuntime>> pending;
  {
    std::lock_guard lock(detector_mutex_);
    if (auto it = runtimes_.find(key); it != runtimes_.end()) {
      return it->second;
    }
    if (auto it = loading_.find(key); it != loading_.end()) {
      // Another thread is already loading this model: wait for its result
      // instead of loading (and briefly doubling GPU memory) twice.
      pending = it->second;
    } else {
      loader = std::make_shared<std::promise<std::shared_ptr<ModelRuntime>>>();
      pending = loader->get_future().share();
      loading_.emplace(key, pending);
    }
  }
  if (!loader) {
    return pending.get();
  }

  // Load sessions without holding the pool lock.
  std::shared_ptr<ModelRuntime> runtime;
  try {
    const int session_count = use_accelerator_
                                  ? workers_per_model_accelerated_
                                  : workers_per_model_;
    runtime = std::make_shared<ModelRuntime>(
        (model_path_ / file).string(), use_accelerator_, target_id,
        session_count, shared_result_wait_);
  } catch (...) {
    {
      std::lock_guard lock(detector_mutex_);
      loading_.erase(key);
    }
    loader->set_exception(std::current_exception());
    throw;
  }

  {
    std::lock_guard lock(detector_mutex_);
    runtimes_.emplace(key, runtime);
    loading_.erase(key);
  }
  loader->set_value(runtime);
  return runtime;
}

DetectorPool::AddTaskFunc DetectorPool::RegisterToDetector(
    Task task, unsigned long id, Callback cb, size_t depth) {
  const auto spec = ModelForTask(task);
  if (!spec) {
    return [](unsigned long, unsigned long, unsigned long, ImagePtr,
              std::shared_ptr<SharedInference>) {};
  }

  std::shared_ptr<YOLODetector> session;
  try {
    auto runtime = GetOrCreateRuntime(spec->file, spec->target_id);
    session = runtime->Register(id, std::move(cb), depth);
  } catch (const std::exception& e) {
    LOGE("Failed to register detector for task {}: {}", toString(task),
         e.what());
    return [](unsigned long, unsigned long, unsigned long, ImagePtr,
              std::shared_ptr<SharedInference>) {};
  }
  if (!session) {
    LOGE("Failed to create or get detector for task {}", toString(task));
    return [](unsigned long, unsigned long, unsigned long, ImagePtr,
              std::shared_ptr<SharedInference>) {};
  }

  return [session](unsigned long id1, unsigned long id2, unsigned long id3,
                   ImagePtr image, std::shared_ptr<SharedInference> shared) {
    session->AddTask({{id1, id2, id3}, std::move(image), std::move(shared)});
  };
}

bool DetectorPool::DeregisterToDetector(Task task, unsigned long id) {
  const auto spec = ModelForTask(task);
  if (!spec) {
    return false;
  }
  std::shared_ptr<ModelRuntime> runtime;
  {
    std::lock_guard lock(detector_mutex_);
    auto it = runtimes_.find(RuntimeKey(spec->file, spec->target_id));
    if (it == runtimes_.end()) {
      return false;
    }
    runtime = it->second;
  }
  return runtime->Deregister(id);
}

void DetectorPool::StopAllDetectors() {
  std::vector<std::shared_ptr<ModelRuntime>> runtimes;
  {
    std::lock_guard lock(detector_mutex_);
    runtimes.reserve(runtimes_.size());
    for (auto& [key, runtime] : runtimes_) {
      runtimes.push_back(runtime);
    }
  }
  for (auto& runtime : runtimes) {
    runtime->StopAll();
  }
}

nlohmann::json DetectorPool::GetModelStatus() const {
  nlohmann::json models = nlohmann::json::array();
  std::lock_guard lock(detector_mutex_);
  for (const auto& [key, runtime] : runtimes_) {
    models.push_back({{"model", runtime->model_file()},
                      {"targetId", runtime->target_id()},
                      {"sessions", runtime->session_count()},
                      {"sharedResultWaitMs", runtime->shared_result_wait().count()},
                      {"running", runtime->isRunning()}});
  }
  return models;
}

}  // namespace avs_analyzer
