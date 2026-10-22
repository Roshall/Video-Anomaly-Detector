#include "server_apis/api_common.h"

#include <string>

#include "metrics.h"

void ApiIndex(evhttp_request* req, void*) {
  nlohmann::json result{ "urls", 
    {
      {"/api", "this api version 2.0"},
      {"/api/health", "check health"},
      {"/api/metrics", "runtime counters for workers, models and alarms"},
      {"/api/controls", "get all controls being analyzed"},
      {"/api/control", "get control being analyzed"},
      {"/api/control/add", "add control"},
      {"/api/control/cancel", "cancel control"},
      {"/api/probe_rtsp", "probe RTSP stream"},
      {"/api/snapshot", "capture snapshot"},
      {"/api/cleanup_files", "remove files under upload_dir"},
      {"/api/storage_usage", "storage usage for upload_dir"},
      {"/api/clip", "clip a video under upload_dir"},
      {"/file/upload", "upload files"},
      {"/file", "serve static files"},
      {"/alarm-sound", "alarm sound control"},
      {"/api/offline/finishedAck", "offline video completion acknowledgment"},
      {"/test/load_models", "test if models are loaded successfully"}
    }
  };
  send_json(req, HTTP_OK, result.dump());
}

void ApiHealth(evhttp_request* req, void*) {
  int result_code = 0;
  std::string result_msg = "error";

  result_code = 1000;
  result_msg = "current service health";

  nlohmann::json result;
  result["msg"] = result_msg;
  result["code"] = result_code;
  send_json(req, HTTP_OK, result.dump());
}

void ApiMetrics(evhttp_request* req, void*) {
  const auto metrics = avs_analyzer::MetricsRegistry::Instance().ToJson();
  nlohmann::json result;
  result["code"] = 1000;
  result["msg"] = "runtime metrics";
  result["data"] = metrics;
  send_json(req, HTTP_OK, result.dump());
}

void ApiOfflineFinishedAck(evhttp_request* req, void*) {
  send_json(req, HTTP_OK, R"({"code":1000,"msg":"success"})");
}
