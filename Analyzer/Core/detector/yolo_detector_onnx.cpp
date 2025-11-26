//
// Created by lg on 10/11/25.
//
#include "yolo_detector_onnx.h"
#include <mutex>

namespace avs_analyzer {
YOLODetector::YOLODetector(const DL_INIT_PARAM& params)
    : params_(params), model_(std::make_unique<YOLOv8Detect>()) {
  model_->CreateSession(params_);
}

void YOLODetector::AddTask(unsigned long id1, unsigned long id2,
                           const cv::Mat& image) {
  DropOverflowTasks();
  task_queue_.enqueue(Task{{id1, id2}, image});
  task_sem_.release();
}

void YOLODetector::RegisterCallback(unsigned long id1, unsigned long id2,
                                    Callback cb) {
  std::unique_lock<std::shared_mutex> lock(callback_mutex_);
  callbacks_[{id1, id2}] = std::move(cb);
}

void YOLODetector::DeregisterCallback(unsigned long id1, unsigned long id2) {
  std::unique_lock<std::shared_mutex> lock(callback_mutex_);
  callbacks_.erase({id1, id2});
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

    auto filtered = FilterResults(model_->RunSession(task.image));
    InvokeCallback(task.id, filtered);
  }
}

std::vector<DL_RESULT> YOLODetector::Run(const cv::Mat& image) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto res = model_->RunSession(image);
  return FilterResults(std::move(res));
}

void YOLODetector::Stop() {
  running_.store(false, std::memory_order_release);
  task_sem_.release();
}

void YOLODetector::DropOverflowTasks() {
  while (task_queue_.size_approx() >= kMaxQueueSize) {
    Task dropped;
    if (!task_queue_.try_dequeue(dropped)) {
      break;
    }
    task_sem_.try_acquire();
    InvokeCallback(dropped.id, {});
  }
}

void YOLODetector::InvokeCallback(const ResourceID& id,
                                  const std::vector<DL_RESULT>& results) const {
  std::shared_lock<std::shared_mutex> lock(callback_mutex_);
  if (auto it = callbacks_.find(id); it != callbacks_.end()) {
    it->second(results);
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
}
}  // namespace avs_analyzer