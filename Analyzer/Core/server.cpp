#include "server.h"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <json/json.h>
#include <json/value.h>

#include <thread>

#include "config.h"
#include "control.h"
#include "scheduler.h"
#include "Utils/common.h"
#include "Utils/log.h"
#include "task.h"
#include <sstream>

int extract_port(const std::string& ip_address) {
  std::string port_str = ip_address.substr(ip_address.rfind(':') + 1);
  int port = 0;
  std::istringstream(port_str) >> port;
  return port;
}

using namespace avs_analyzer;

constexpr int kRecvBufMaxSize = 1024 * 8;

std::thread HTTPServerStart(avs_analyzer::Scheduler& scheduler, event_base* base) {
  int port = extract_port(scheduler.GetConfig().analyzer_host);
  return std::thread([&, base, port]() {
    LOGI("启动分析器服务：http://0.0.0.0:%d", port);
//    event_config* evt_config = event_config_new();
//    event_base* base = event_base_new_with_config(evt_config);
    evhttp* http = evhttp_new(base);
    evhttp_set_default_content_type(http, "text/html; charset=utf-8");

    evhttp_set_timeout(http, 0);
    // 设置路由
    evhttp_set_cb(http, "/", ApiIndex, nullptr);
    evhttp_set_cb(http, "/api/health", ApiHealth, nullptr);
    evhttp_set_cb(http, "/api/controls", ApiAllMonitors, &scheduler);
    evhttp_set_cb(http, "/api/control", ApiMonitor, &scheduler);
    evhttp_set_cb(http, "/api/control/add", ApiAddMonitor, &scheduler);
    evhttp_set_cb(http, "/api/control/cancel", ApiRemoveMonitor, &scheduler);

    evhttp_bind_socket(http, "0.0.0.0", port);
    event_base_dispatch(base);

    evhttp_free(http);
//    event_config_free(evt_config);
//    event_base_free(base);
  });
}

static inline bool to_int(const std::string& s, int& out) {
  auto* begin = s.data(); auto* end = s.data() + s.size();
  auto rc = std::from_chars(begin, end, out);
  return rc.ec == std::errc() && rc.ptr == end;
}

static inline int get_int_any(const Json::Value& v, int def = 0) {
  if (v.isInt()) return v.asInt();
  if (v.isUInt()) return static_cast<int>(v.asUInt());
  if (v.isString()) { int x{}; if (to_int(v.asString(), x)) return x; }
  if (v.isDouble()) return static_cast<int>(v.asDouble());
  return def;
}

static inline std::string get_string(const Json::Value& v, const std::string& def = "") {
  return v.isString() ? v.asString() : def;
}

static std::pair<std::vector<int>, std::vector<std::string>> parse_levels(const Json::Value& levelsNode, int& mode) {
  std::vector<int> thresholds;
  std::vector<std::string> levels;

  if (!levelsNode || !levelsNode.isObject()) return { thresholds, levels};

  // Iterate through all levels
  std::vector<std::pair<int, std::string>> combined;
  const auto keys = levelsNode.getMemberNames();
  for (auto& name : keys) {
    const Json::Value& lv = levelsNode[name];
    if (!lv.isObject()) continue;

    int threshold = get_int_any(lv["threshold"], 0);
    mode = get_string(lv["unit"], "") == "seconds"? 1: 0;

    combined.emplace_back(threshold, name);
  }


  // Sort by threshold
  std::sort(combined.begin(), combined.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;  // Ascending order by threshold
  });
  for (const auto& [threshold, unit] : combined) {
    thresholds.push_back(threshold);
    levels.push_back(unit);
  }

  return { thresholds, levels};
}

void ApiIndex(evhttp_request* req, void* arg) {
  Json::Value result_urls;
  result_urls["/api"] = "this api version 1.0";
  result_urls["/api/health"] = "check health";
  result_urls["/api/controls"] = "get all control being analyzed";
  result_urls["/api/control"] = "get control being analyzed";
  result_urls["/api/control/add"] = "add control";
  result_urls["/api/control/cancel"] = "cancel control";
  result_urls["/api/largeModelCalcu"] = "largeModelCalcu";

  Json::Value result;
  result["urls"] = result_urls;

  evbuffer* buff = evbuffer_new();
  evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}
void ApiHealth(evhttp_request* req, void* arg) {
  int result_code = 0;
  std::string result_msg = "error";

  // 健康检测
  result_code = 1000;
  result_msg = "current service health";

  Json::Value result;
  result["msg"] = result_msg;
  result["code"] = result_code;

  evbuffer* buff = evbuffer_new();
  evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}
void ApiAllMonitors(evhttp_request* req, void* arg) {
  auto scheduler = static_cast<Scheduler*>(arg);
  char buf[kRecvBufMaxSize];
  ParsePostRequest(req, buf);

  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  JSONCPP_STRING errs;

  int result_code = 0;
  std::string result_msg = "error";
  Json::Value result;

  if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) &&
      errs.empty()) {
    if (auto monitors = scheduler->GetAllMonitors(); !monitors.empty()) {
      Json::Value result_data_item;
      Json::Value result_data;
      int64_t curTimestamp = getCurTimestamp();
      int64_t startTimestamp = 0;
      for (auto& monitor : monitors) {
        startTimestamp = monitor.start_timestamp;

        result_data_item["code"] = monitor.code.data();
        result_data_item["streamUrl"] = monitor.stream_url.data();
        result_data_item["pushStream"] = monitor.push_stream;
        result_data_item["pushStreamUrl"] = monitor.push_stream_url.data();
        result_data_item["algorithmCode"] = toString(monitor.task);
        result_data_item["recognitionRegion"] = monitor.roi_str.data();
        result_data_item["sourceType"] = monitor.source_type;
        result_data_item["startTimestamp"] = startTimestamp;
        result_data_item["liveMilliseconds"] = curTimestamp - startTimestamp;

        result_data.append(result_data_item);
      }
      result["data"] = result_data;
      result_code = 1000;
      result_msg = "success";
    } else {
      result_msg = "the number of control exector is empty";
    }

  } else {
    result_msg = "invalid request parameter";
  }
  result["msg"] = result_msg;
  result["code"] = result_code;

  // LOGI("\n \t request:%s \n \t response:%s", root.toStyledString().data(),
  // result.toStyledString().data());

  evbuffer* buff = evbuffer_new();
  evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}
void ApiMonitor(evhttp_request* req, void* arg) {
  auto scheduler = static_cast<Scheduler*>(arg);
  char buf[kRecvBufMaxSize];
  ParsePostRequest(req, buf);

  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  JSONCPP_STRING errs;

  Json::Value result_monitor;
  int result_code = 0;
  std::string result_msg = "error";

  if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) &&
      errs.empty()) {
    Monitor* monitor = nullptr;
    if (root["code"].isString()) {
      std::string code = root["code"].asCString();
      monitor = scheduler->GetMonitorByCode(code);
    }

    if (monitor) {
      result_monitor["code"] = monitor->code;

      result_code = 1000;
      result_msg = "success";

    } else {
      result_msg = "the monitor does not exist";
    }
  } else {
    result_msg = "invalid request parameter";
  }

  Json::Value result;
  result["control"] = result_monitor;
  result["msg"] = result_msg;
  result["code"] = result_code;

  LOGI("\n \t request:%s \n \t response:%s", root.toStyledString().data(),
       result.toStyledString().data());

  evbuffer* buff = evbuffer_new();
  evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}
void ApiAddMonitor(evhttp_request* req, void* arg) {
  const auto scheduler = static_cast<Scheduler*>(arg);
  char buf[kRecvBufMaxSize];
  ParsePostRequest(req, buf);

  const Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  JSONCPP_STRING errs;

  int result_code = 0;
  std::string result_msg;

  if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) &&
      errs.empty()) {
    try {
      auto task = std::stoi(root["algorithmCode"].asCString());
      if (task < 0 || task >= 6) {
        LOGE("wrong algorithm code: %d", task);
        return;
      }

      int mode;
      Monitor monitor{
          .code = root["code"].asCString(),
          .source_type = root["sourceType"].asCString(),
          .stream_url = root["streamUrl"].asString(),
          .push_stream = root["pushStream"].asBool(),
          .push_stream_url = root["pushStreamUrl"].asString(),
          .task = static_cast<Task>(task),
          .roi_str = root["recognitionRegion"].asString(),
          .levels = parse_levels(root["levels"], mode),
      };
      monitor.level_mode = static_cast<LevelMode>(mode);
      if (monitor.ValidateAdd(result_msg)) {
        scheduler->MonitorAdd(std::make_unique<Monitor>(std::move(monitor)), result_code, result_msg);
      } else {
        LOGE("invalid message.");
        return;
      }

    } catch (const std::exception& e) {
      LOGE("Json parsing error:%s", e.what());
      return;
    }

  } else {
    result_msg = "invalid request parameter";
  }

  Json::Value result;
  result["msg"] = result_msg;
  result["code"] = result_code;

  LOGI("\n \t request:%s \n \t response:%s", root.toStyledString().data(),
       result.toStyledString().data());

  evbuffer* buff = evbuffer_new();
  evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}

void ApiRemoveMonitor(evhttp_request* req, void* arg) {
  auto scheduler = static_cast<Scheduler*>(arg);
  char buf[kRecvBufMaxSize];
  ParsePostRequest(req, buf);

  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

  Json::Value root;
  JSONCPP_STRING errs;

  int result_code = 0;
  std::string result_msg = "error";

  if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) &&
      errs.empty()) {
    std::string code;

    if (root["code"].isString()) {
      code = root["code"].asCString();
    }
    if (!code.empty()) {
      scheduler->MonitorRemove(code, result_code, result_msg);
    } else {
      result_msg = "validate parameter error";
    }

  } else {
    result_msg = "invalid request parameter";
  }

  Json::Value result;
  result["msg"] = result_msg;
  result["code"] = result_code;

  LOGI("\n \t request:%s \n \t response:%s", root.toStyledString().data(),
       result.toStyledString().data());

  evbuffer* buff = evbuffer_new();
  evbuffer_add_printf(buff, "%s", result.toStyledString().c_str());
  evhttp_send_reply(req, HTTP_OK, nullptr, buff);
  evbuffer_free(buff);
}
void ParseGetRequest(const evhttp_request* req, evkeyvalq* params) {
  if (req == nullptr) {
    return;
  }
  const char* url = evhttp_request_get_uri(req);
  if (url == nullptr) {
    return;
  }
  const evhttp_uri* decoded = evhttp_uri_parse(url);
  if (!decoded) {
    return;
  }
  // if (const char* path = evhttp_uri_get_path(decoded); path == nullptr) {
  //   path = "/";
  // }
  const char* query = const_cast<char*>(evhttp_uri_get_query(decoded));
  if (query == nullptr) {
    return;
  }
  evhttp_parse_query_str(query, params);
}
void ParsePostRequest(const evhttp_request* req, char* buff) {
  size_t post_size = 0;

  post_size = evbuffer_get_length(req->input_buffer);
  if (post_size <= 0) {
    //        printf("====line:%d,post msg is empty!\n",__LINE__);
    return;
  }
  const size_t copy_len =
      post_size > kRecvBufMaxSize ? kRecvBufMaxSize : post_size;
  //        printf("====line:%d,post len:%d,
  //        copy_len:%d\n",__LINE__,post_size,copy_len);
  memcpy(buff, evbuffer_pullup(req->input_buffer, -1), copy_len);
  buff[post_size] = '\0';
  //        printf("====line:%d,post msg:%s\n",__LINE__,buf);
}
