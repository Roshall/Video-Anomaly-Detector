#include "server_apis/api_common.h"

#include <string>

void ApiIndex(evhttp_request* req, void*) {
  nlohmann::json result_urls;
  result_urls["/api"] = "this api version 1.0";
  result_urls["/api/health"] = "check health";
  result_urls["/api/controls"] = "get all control being analyzed";
  result_urls["/api/control"] = "get control being analyzed";
  result_urls["/api/control/add"] = "add control";
  result_urls["/api/control/cancel"] = "cancel control";
  result_urls["/api/largeModelCalcu"] = "largeModelCalcu";
  result_urls["/api/cleanup_files"] = "remove files under upload_dir";
  result_urls["/api/storage_usage"] = "storage usage for upload_dir";
  result_urls["/api/clip"] = "clip a video under upload_dir";

  nlohmann::json result;
  result["urls"] = result_urls;
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
