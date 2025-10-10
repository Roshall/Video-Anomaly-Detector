#include "scheduler.h"

#include <fstream>
#include <mutex>
#include <string>

#include "Utils/log.h"
#include "algorithm.h"
#include "algorithm_on_yolo.h"
#include "config.h"
#include "control.h"
#include "alarm.h"
#include "worker.h"



namespace avs_analyzer {
Scheduler::Scheduler(Config& config) : config_(config) {}

Scheduler::~Scheduler() = default;

Config& Scheduler::GetConfig() const { return config_; }
bool Scheduler::InitAlgorithm() {
  LOGI("initAlgorithm() start");

  // 初始化on-yolo8 80分类模型
  LOGI("初始化on-yolo8 80分类模型");
  std::string model_path = config_.model_dir + "/yolov8n.onnx";
  std::vector<std::string> class_names = {
      "person",        "bicycle",      "car",
      "motorcycle",    "airplane",     "bus",
      "train",         "truck",        "boat",
      "traffic light", "fire hydrant", "stop sign",
      "parking meter", "bench",        "bird",
      "cat",           "dog",          "horse",
      "sheep",         "cow",          "elephant",
      "bear",          "zebra",        "giraffe",
      "backpack",      "umbrella",     "handbag",
      "tie",           "suitcase",     "frisbee",
      "skis",          "snowboard",    "sports ball",
      "kite",          "baseball bat", "baseball glove",
      "skateboard",    "surfboard",    "tennis racket",
      "bottle",        "wine glass",   "cup",
      "fork",          "knife",        "spoon",
      "bowl",          "banana",       "apple",
      "sandwich",      "orange",       "broccoli",
      "carrot",        "hot dog",      "pizza",
      "donut",         "cake",         "chair",
      "couch",         "potted plant", "bed",
      "dining table",  "toilet",       "tv",
      "laptop",        "mouse",        "remote",
      "keyboard",      "cell phone",   "microwave",
      "oven",          "toaster",      "sink",
      "refrigerator",  "book",         "clock",
      "vase",          "scissors",     "teddy bear",
      "hair drier",    "toothbrush"};
  yolov8n_80_classes_ =
      std::make_unique<YoloAlgorithm>(&config_, model_path, class_names);


  LOGI("initAlgorithm() end");
  return true;
}

void Scheduler::CleanUpLoop() {
  // TODO: use thread pool to handle alarm and cleanup
  while (state_) {
    {
      std::unique_lock lk(clean_up_mtx_);
      clean_up_cv_.wait(lk, [this] {
        return !this->state_ ||
               this->alarm_queue_.size_approx() > 0 ||
               this->to_be_deleted_workers_queue_.size_approx() > 0;
      });
    }
    if (!state_) break;
//    PullAndGenAlarm();
    {
      std::unique_ptr<Alarm> alarm;
      while (alarm_queue_.try_dequeue(alarm)) {
        alarm->Finalize(config_);
      }
    }
    std::unique_ptr<Worker> worker;
    while (to_be_deleted_workers_queue_.try_dequeue(worker)) {
    }
  }
}

std::vector<Monitor> Scheduler::GetAllMonitors() {
  std::vector<Monitor> monitors;
  {
    std::lock_guard lock(worker_map_mutex_);
    monitors.reserve(worker_map_.size());
    for (auto& pair : worker_map_) {
      monitors.push_back(*pair.second->monitor_.get());
    }
  }

  return monitors;
}
Monitor* Scheduler::GetMonitorByCode(const std::string& code) {
  Monitor* monitor = nullptr;
  {
    std::lock_guard lock(worker_map_mutex_);
    if (const auto it = worker_map_.find(code); it != worker_map_.end()) {
      monitor = it->second->monitor_.get();
    }
  }

  return monitor;
}

void Scheduler::MonitorAdd(std::unique_ptr<Monitor> monitor, int& result_code,
                           std::string& result_msg) {
  const std::string& code = monitor->code;
  if (Contains(code)) {
    result_msg = "the control is running";
    result_code = 1000;
    return;
  }

  auto worker = std::make_unique<Worker>(this, std::move(monitor));
  if (worker->start(result_msg)) {
    if (AddWorker(std::move(worker))) {
      result_msg = "add success";
      result_code = 1000;
    } else {
      result_msg = "add error";
      result_code = 0;
    }
  } else {
    result_code = 0;
  }
}
void Scheduler::MonitorRemove(const std::string& code, int& result_code,
                              std::string& result_msg) {
  Worker* worker = GetWorkerByCode(code);

  if (worker) {
    if (worker->getState()) {
      result_msg = "control is running, ";
    } else {
      result_msg = "control is not running, ";
    }

    RemoveWorker(code);

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
bool Scheduler::Contains(const std::string& code) {
  std::lock_guard lock(worker_map_mutex_);
  return worker_map_.end() != worker_map_.find(code);
}
bool Scheduler::AddWorker(std::unique_ptr<Worker> worker) {
  bool added = false;
  const std::string& code = worker->monitor_->code;
  {
    std::lock_guard lock(worker_map_mutex_);
    if (bool is_already_added = worker_map_.end() != worker_map_.find(code);
        !is_already_added) {
      worker_map_.emplace(code, std::move(worker));
      added = true;
    }
  }
  return added;
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
Worker* Scheduler::GetWorkerByCode(const std::string& code) {
  Worker* found_worker = nullptr;
  {
    std::lock_guard lock(worker_map_mutex_);
    if (const auto f = worker_map_.find(code); worker_map_.end() != f) {
      found_worker = f->second.get();
    }
  }
  return found_worker;
}

void Scheduler::NotifyCleanUp() {
  std::lock_guard lock(clean_up_mtx_);
  clean_up_cv_.notify_one();
}


void Scheduler::AddAlarm(std::unique_ptr<Alarm> alarm) {
  // TODO: alarm queue size to be configurable
  const int kAlarmQueueSize = 100;

  std::unique_ptr<Alarm> old_alarm;
  alarm_queue_.enqueue(std::move(alarm));
  NotifyCleanUp();
}
}  // namespace avs_analyzer
