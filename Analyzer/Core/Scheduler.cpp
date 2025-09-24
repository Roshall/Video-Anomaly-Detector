#include "Scheduler.h"
#include "Config.h"
#include "Control.h"
#include "Worker.h"
#include "Algorithm.h"
#include "AlgorithmOnYolo.h"
#include "GenerateAlarmVideo.h"
#include "Utils/Log.h"
#include <fstream>
#include <string>

namespace AVSAnalyzer {
    Scheduler::Scheduler(Config* config) :mConfig(config), mState(false)
    {
        LOGI("");

    }

    Scheduler::~Scheduler()
    {
        LOGI("");

        // clearAlarmQueue(); // AlarmQueue store smart pointers, they will clean up the resource themselves.
        if (mLoopAlarmThread) {
            mLoopAlarmThread->join();
        }
    }

    Config* Scheduler::getConfig() const {
        return mConfig;
    }
    bool Scheduler::initAlgorithm() {
        LOGI("initAlgorithm() start");

        //初始化on-yolo8 80分类模型
        LOGI("初始化on-yolo8 80分类模型");
        std::string modelPath = mConfig->model_dir + "/yolov8n.onnx";
        std::vector<std::string> classNames = {
            "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
            "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
            "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
            "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
            "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
            "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
            "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
            "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
            "hair drier", "toothbrush" };
        on_yolov8n_80 = std::make_unique<AlgorithmOnYolo>(mConfig, modelPath,classNames);

        //auto img = cv::imread("bus.jpg");
        //if (!img.empty()) {
        //    std::vector<DetectObject> detects;
        //    algorithm_openvino_yolov8n->objectDetect(img, detects);
        //    LOGI("img.w=%d,img.h=%d,detects.size()=%d", img.cols,img.rows,detects.size());

        //}


        LOGI("initAlgorithm() end");
        return true;
    }

    void Scheduler::StartAlarmThread() {
        mLoopAlarmThread = std::make_unique<std::thread>(loopAlarmThread, this);
    }
    void Scheduler::CleanWorkerLoop() {

        mLoopAlarmThread = std::make_unique<std::thread>(loopAlarmThread, this);
        LOGI("Start Success");
        while (mState)
        {
            // TODO: wait, and use control variable to wake up this thread.
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            std::unique_ptr<Worker> worker[10];
            mTobeDeletedWorkerQ.try_dequeue_bulk(worker, 10);
        }

    }

    std::vector<Monitor*> Scheduler::GetAllMonitors() {
        std::vector<Monitor*> monitors;
        {
            std::lock_guard lock(mWorkerMapMtx);
            monitors.reserve(mWorkerMap.size());
            for (auto& pair : mWorkerMap)
            {
                monitors.push_back(pair.second->mMonitor);
            }
        }

        return monitors;
    }
    Monitor* Scheduler::GetMonitorByCode(const std::string& code) {
        Monitor* control = nullptr;
        {
            std::lock_guard lock(mWorkerMapMtx);
            if (const auto it = mWorkerMap.find(code); it != mWorkerMap.end()) {
                control = it->second->mMonitor;
            }
        }

        return control;
    }


    void Scheduler::MonitorAdd(Monitor* monitor, int& result_code, std::string& result_msg) {

        if (isAdd(monitor)) {
            result_msg = "the control is running";
            result_code = 1000;
            return;
        }

        if (auto worker = std::make_unique<Worker>(this, monitor); worker->start(result_msg)) {
            if (addWorker(monitor, std::move(worker))) {
                result_msg = "add success";
                result_code = 1000;
            }
            else {
                result_msg = "add error";
                result_code = 0;
            }
        }
        else {
            result_code = 0;
        }
    }
    void Scheduler::MonitorRemove(Monitor* monitor, int& result_code, std::string& result_msg) {

        Worker* worker = GetWorkerByCode(monitor->code);

        if (worker) {
            if (worker->getState()) {
                result_msg = "control is running, ";
            }
            else {
                result_msg = "control is not running, ";
            }

            removeWorker(monitor->code);

            result_msg += "cancel success";
            result_code = 1000;
            return;

        }
        result_msg = "there is no such control";
        result_code = 0;
   }
    void Scheduler::setState(const bool state) {
        mState = state;
    }
    bool Scheduler::getState() const {
        return mState;
    }

    int Scheduler::getWorkerSize() {
        size_t size{};
        {
            std::lock_guard lock(mWorkerMapMtx);
            size = mWorkerMap.size();
        }

        return static_cast<int>(size);
    }
    bool Scheduler::isAdd(Monitor* control) {
        bool is_add{};
        {
            std::lock_guard lock(mWorkerMapMtx);
            is_add = mWorkerMap.end() != mWorkerMap.find(control->code);
        }

        return is_add;
    }
    bool Scheduler::addWorker(Monitor* control, std::unique_ptr<Worker> worker) {
        bool add = false;

        {
            std::lock_guard lock(mWorkerMapMtx);
            if (bool isAdd = mWorkerMap.end() != mWorkerMap.find(control->code); !isAdd) {
                mWorkerMap.emplace(control->code, std::move(worker));
                add = true;
            }
        }
        return add;
    }
    bool Scheduler::removeWorker(const std::string& code) {
        bool result = false;
        std::unique_ptr<Worker> worker;
        {
            std::lock_guard lock(mWorkerMapMtx);
            if (auto f = mWorkerMap.find(code); mWorkerMap.end() != f) {
                worker = std::move(f->second);
                mWorkerMap.erase(f);
                result = true;
            }
        }
        if (result) {
            mTobeDeletedWorkerQ.enqueue(std::move(worker));
        }
        return result;
    }
    Worker* Scheduler:: GetWorkerByCode(const std::string& code) {
        Worker* worker = nullptr;
        {
            std::lock_guard lock(mWorkerMapMtx);
            if (const auto f = mWorkerMap.find(code); mWorkerMap.end() != f) {
                worker = f->second.get();
            }
        }
        return worker;
    }


    void Scheduler::PullAndGenAlarmLoop() {
        // TODO: control variables to wake up this thread
        while (true) {
            std::unique_ptr<Alarm> alarm;
            if (mAlarmQ.try_dequeue(alarm)) {
                GenerateAlarmVideo gen(mConfig, alarm.get());
                gen.genAlarmVideo();
            }
        }
    }
    void Scheduler::loopAlarmThread(void* arg) {
        auto scheduler = static_cast<Scheduler *>(arg);
        
        scheduler->PullAndGenAlarmLoop();
    }

    void Scheduler::addAlarm(std::unique_ptr<Alarm> alarm) {
        // TODO: if the alarmQ get too big, we might need to notify the detector, reduce FPS
        std::unique_ptr<Alarm> old_alarm;
        mAlarmQ.enqueue(std::move(alarm));
    }

    // void Scheduler::clearAlarmQueue() {
    //     // swap isn't thread free, but we are pretty sure that no other thread are alive if this function gets called.
    // }

}
