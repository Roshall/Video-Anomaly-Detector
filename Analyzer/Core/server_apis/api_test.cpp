#include "server_apis/api_test.h"

#include <exception>
#include <utility>

#include "config.h"
#include "detector/constant_yolo.h"
#include "detector/yolo_detector_onnx.h"
#include "scheduler.h"
#include "server_apis/api_common.h"
#include "task.h"

// Load each unique model once through a temporary DetectorPool.
// A no-op callback is registered so that DetectorPool::RegisterToDetector
// triggers the underlying YOLODetector construction (and thus ONNX model
// loading) without affecting the production detector pool held by Scheduler.
// The temporary pool is destroyed at the end of the handler, which stops and
// joins all detector threads created during the test.
void ApiTestLoadModels(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<avs_analyzer::Scheduler*>(arg);
  const auto& config = scheduler->GetConfig();

  // One representative task per unique model file in constant_yolo.cpp.
  static const std::pair<avs_analyzer::Task, const char*> kModelTests[] = {
      {avs_analyzer::Task::kBreakIn,      "yolov8n.onnx"},
      {avs_analyzer::Task::kSleepWork,    "sleeping_m.onnx"},
      {avs_analyzer::Task::kWallClimbing, "climbing_m.onnx"},
      {avs_analyzer::Task::kPhonePlaying, "phone_playing_m.onnx"},
  };

  avs_analyzer::YOLODetector::Callback noop_cb{
      [](const avs_analyzer::YOLODetector::ResultType&, uint64_t) {},
      []() -> uint64_t { return 0; }};

  nlohmann::json model_results = nlohmann::json::array();
  bool all_ok = true;

  {
    avs_analyzer::DetectorPool pool(config.model_dir, config.use_accelerator);
    for (const auto& [task, model_name] : kModelTests) {
      nlohmann::json entry;
      entry["model"] = model_name;
      entry["task"] = avs_analyzer::toString(task);
      try {
        pool.RegisterToDetector(task, 0, noop_cb);
        entry["status"] = "ok";
      } catch (const std::exception& e) {
        entry["status"] = "failed";
        entry["error"] = e.what();
        all_ok = false;
      }
      model_results.push_back(entry);
    }
  }  // pool destructs here — stops and joins all detector threads
  if (all_ok) {
    LOGD("All models loaded successfully");
  } else {
    LOGD("Some models failed to load");
  }
  nlohmann::json resp;
  resp["code"] = all_ok ? 1000 : 0;
  resp["msg"] = all_ok ? "all models loaded successfully" : "some models failed to load";
  resp["models"] = model_results;
  send_json(req, HTTP_OK, resp.dump());
}
