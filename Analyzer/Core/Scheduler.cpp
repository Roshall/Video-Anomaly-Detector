#include "Scheduler.h"

#include <fstream>
#include <string>

#include "Algorithm.h"
#include "AlgorithmOnYolo.h"
#include "Config.h"
#include "Control.h"
#include "GenerateAlarmVideo.h"
#include "Utils/Log.h"
#include "Worker.h"

namespace avs_analyzer {
Scheduler::Scheduler(Config* config) : config_(config), state_(false) {
  LOGI("");
}

Scheduler::~Scheduler() {
  LOGI("");

  // clearAlarmQueue(); // AlarmQueue store smart pointers, they will clean up
  // the resource themselves.
  if (alarm_thread_) {
    alarm_thread_->join();
  }
}

Config* Scheduler::GetConfig() const { return config_; }
bool Scheduler::InitAlgorithm() {
  LOGI("initAlgorithm() start");

  // 初始化on-yolo8 80分类模型
  LOGI("初始化on-yolo8 80分类模型");
  std::string model_path = config_->model_dir + "/yolov8n.onnx";
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
      std::make_unique<YoloAlgorithm>(config_, model_path, class_names);

  // auto img = cv::imread("bus.jpg");
  // if (!img.empty()) {
  //     std::vector<DetectObject> detects;
  //     algorithm_openvino_yolov8n->objectDetect(img, detects);
  //     LOGI("img.w=%d,img.h=%d,detects.size()=%d",
  //     img.cols,img.rows,detects.size());

  //}

  LOGI("initAlgorithm() end");
  return true;
}

void Scheduler::StartAlarmThread() {
  alarm_thread_ = std::make_unique<std::thread>(AlarmLoopThread, this);
}
void Scheduler::CleanWorkerLoop() {
  alarm_thread_ = std::make_unique<std::thread>(AlarmLoopThread, this);
  LOGI("Start Success");
  while (state_) {
    // TODO: wait, and use control variable to wake up this thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    std::unique_ptr<Worker> workers[10];
    to_be_deleted_workers_queue_.try_dequeue_bulk(workers, 10);
  }
}

std::vector<Monitor*> Scheduler::GetAllMonitors() {
  std::vector<Monitor*> monitors;
  {
    std::lock_guard lock(worker_map_mutex_);
    monitors.reserve(worker_map_.size());
    for (auto& pair : worker_map_) {
      monitors.push_back(pair.second->monitor.get());
    }
  }

  return monitors;
}
Monitor* Scheduler::GetMonitorByCode(const std::string& code) {
  Monitor* monitor = nullptr;
  {
    std::lock_guard lock(worker_map_mutex_);
    if (const auto it = worker_map_.find(code); it != worker_map_.end()) {
      monitor = it->second->monitor.get();
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

int Scheduler::GetWorkerSize() {
  size_t num_workers{};
  {
    std::lock_guard lock(worker_map_mutex_);
    num_workers = worker_map_.size();
  }

  return static_cast<int>(num_workers);
}
bool Scheduler::Contains(const std::string& code) {
  bool is_added{};
  {
    std::lock_guard lock(worker_map_mutex_);
    is_added = worker_map_.end() != worker_map_.find(code);
  }

  return is_added;
}
bool Scheduler::AddWorker(std::unique_ptr<Worker> worker) {
  bool added = false;

  {
    std::lock_guard lock(worker_map_mutex_);
    const std::string& code = worker->monitor->code;
    if (bool is_already_added =
            worker_map_.end() != worker_map_.find(code);
        !is_already_added) {
      worker_map_.emplace(code, std::move(worker));
      added = true;
    }
  }
  return added;
}
bool Scheduler::RemoveWorker(const std::string& code) {
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
  }
  return result;
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

void Scheduler::PullAndGenAlarmLoop() {
  // TODO: control variables to wake up this thread
  while (true) {
    std::unique_ptr<Alarm> alarm;
    if (alarm_queue_.try_dequeue(alarm)) {
      GenerateAlarmVideo gen(config_, alarm.get());
      gen.genAlarmVideo();
    }
  }
}
void Scheduler::AlarmLoopThread(void* arg) {
  auto scheduler = static_cast<Scheduler*>(arg);

  scheduler->PullAndGenAlarmLoop();
}

void Scheduler::AddAlarm(std::unique_ptr<Alarm> alarm) {
  // TODO: if the alarmQ get too big, we might need to notify the detector,
  // reduce FPS
  std::unique_ptr<Alarm> old_alarm;
  alarm_queue_.enqueue(std::move(alarm));
}

// void Scheduler::clearAlarmQueue() {
//     // swap isn't thread free, but we are pretty sure that no other thread
//     are alive if this function gets called.
// }

}  // namespace avs_analyzer
