//
// Created by lg on 10/11/25.
//
#include "yolo_detector_onnx.h"
#include <memory>
#include <mutex>
#include <thread>

#include <Utils/log.h>
namespace avs_analyzer {
YOLODetector::YOLODetector(const DL_INIT_PARAM& params)
    : params_(params), model_(std::make_unique<YOLOv8Detect>()) {
  model_->CreateSession(params_);
  worker_thread_ = std::jthread([this] { Run(); });
}

YOLODetector::~YOLODetector() {
  Stop();
}

void YOLODetector::AddTask(Task task) {
  DropOverflowTasks();
  task_queue_.enqueue(task);
  task_sem_.release();
}

void YOLODetector::RegisterCallback(unsigned long id, Callback cb) {
  std::unique_lock<std::shared_mutex> lock(callback_mutex_);
  callbacks_[id] = std::move(cb);
}

void YOLODetector::DeregisterCallback(unsigned long id) {
  std::unique_lock<std::shared_mutex> lock(callback_mutex_);
  callbacks_.erase(id);
  bool isEmpty = callbacks_.empty();
  if (isEmpty) {
    Stop();
  }
}

void YOLODetector::Run() {
  while (running_.load(std::memory_order_acquire) ||
         task_queue_.size_approx() > 0) {
    task_sem_.acquire();

    Task task;
    if (!task_queue_.try_dequeue(task)) {
      continue;
    }

    if (!task.image || task.image->empty()) {
      InvokeCallback(task.id, {});
      continue;
    }

    auto filtered = FilterResults(model_->RunSession(*task.image));
    InvokeCallback(task.id, filtered);
  }
}
void YOLODetector::Stop() {
  running_.store(false, std::memory_order_release);
  task_sem_.release();
}

void YOLODetector::DropOverflowTasks() {
  while (task_queue_.size_approx() >= kMaxQueueSize) {
    auto num = task_queue_.size_approx() - kMaxQueueSize + 1;
    auto dropped = std::make_unique<Task[]>(num);
    num = task_queue_.try_dequeue_bulk(dropped.get(), num);
    if (num == 0) {
      break;
    }
    LOGW("YOLODetector task queue overflow, dropping {} tasks.", num);
    for (size_t i = 0; i < num; ++i) {
      task_sem_.try_acquire();
      InvokeCallback(dropped[i].id, {});
    }
  }
}

void YOLODetector::InvokeCallback(const ResourceID& id,
                                  const std::vector<DL_RESULT>& results) const {
  std::shared_lock<std::shared_mutex> lock(callback_mutex_);
  if (auto it = callbacks_.find(id[0]); it != callbacks_.end()) {
    // Pass both the result and the frame timestamp explicitly
    it->second({results, id[2]}, static_cast<uint64_t>(id[2]));
  }
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
YOLODetector::YOLODetector(std::string_view mode_path, bool use_accelerator,
                           int target_id)
    : params_(kDefaultParam), model_(std::make_unique<YOLOv8Detect>()) {
  params_.modelPath = mode_path;
  params_.target_id = target_id;
  params_.cudaEnable = use_accelerator;
  model_->CreateSession(params_);
  worker_thread_ = std::jthread([this] { Run(); });
}
}  // namespace avs_analyzer