#include "server.h"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <json/json.h>
#include <json/value.h>

#include <numeric>
#include <sstream>
#include <thread>

#include "Utils/base64.h"
#include "Utils/common.h"
#include "Utils/file_server_cb.h"
#include "Utils/log.h"
#include "alarm_sound.h"
#include "config.h"
#include "event.h"
#include "monitor.h"
#include "scheduler.h"
#include "task.h"

static int extract_port(const std::string& ip_address) {
  std::string port_str = ip_address.substr(ip_address.rfind(':') + 1);
  int port = 0;
  std::istringstream(port_str) >> port;
  return port;
}

using namespace avs_analyzer;

constexpr int kRecvBufMaxSize = 1024 * 8;

std::thread HTTPServerStart(Scheduler& scheduler, event_base* event_base_p,
                            FileRouteOptions& file_route_opt) {
  int port = extract_port(scheduler.GetConfig().analyzer_host);
  return std::thread([&, event_base_p, port]() {
    LOGI("启动分析器服务：http://0.0.0.0:{}", port);
    evhttp* http = evhttp_new(event_base_p);
    evhttp_set_default_content_type(http, "text/html; charset=utf-8");

    evhttp_set_timeout(http, 0);
    // 设置路由
    evhttp_set_cb(http, "/", ApiIndex, nullptr);
    evhttp_set_cb(http, "/api/health", ApiHealth, nullptr);
    evhttp_set_cb(http, "/api/controls", ApiAllMonitors, &scheduler);
    evhttp_set_cb(http, "/api/control", ApiMonitor, &scheduler);
    evhttp_set_cb(http, "/api/control/add", ApiAddMonitor, &scheduler);
    evhttp_set_cb(http, "/api/control/cancel", ApiRemoveMonitor, &scheduler);
    evhttp_set_cb(http, "/api/snapshot", ApiSnapShot, nullptr);
    evhttp_set_cb(http, "/video", file_route_cb, &file_route_opt);
    evhttp_set_cb(http, "/alarm-sound", alarm_sound_cv, scheduler.GetAlarmController());

    evhttp_bind_socket(http, "0.0.0.0", port);
    event_base_dispatch(event_base_p);

    evhttp_free(http);
  });
}

static bool to_int(const std::string& s, int& out) {
  auto* begin = s.data(); auto* end = s.data() + s.size();
  auto rc = std::from_chars(begin, end, out);
  return rc.ec == std::errc() && rc.ptr == end;
}

static int get_int_any(const Json::Value& v, int def = 0) {
  if (v.isInt()) return v.asInt();
  if (v.isUInt()) return static_cast<int>(v.asUInt());
  if (v.isString()) { int x{}; if (to_int(v.asString(), x)) return x; }
  if (v.isDouble()) return static_cast<int>(v.asDouble());
  return def;
}

static std::string get_string(const Json::Value& v, const std::string& def = "") {
  return v.isString() ? v.asString() : def;
}

static std::pair<std::vector<int>, std::vector<std::string>> parse_levels(const Json::Value& levelsNode, LevelMode& mode) {
  std::vector<int> thresholds;
  std::vector<std::string> levels;

  if (!levelsNode || !levelsNode.isObject()) return { thresholds, levels};

  // v2 format: { "mode": "seconds"|"people", "values": [3, 5, ...] }
  if (levelsNode.isMember("values") && levelsNode["values"].isArray()) {
    const auto mode_str = get_string(levelsNode["mode"], "");
    mode = (mode_str == "seconds" || mode_str == "duration")
               ? LevelMode::kDuration
               : LevelMode::kNumPeople;

    const Json::Value& values = levelsNode["values"];
    thresholds.reserve(values.size());
    levels.reserve(values.size());

    for (const auto& v : values) {
      int threshold = get_int_any(v, 0);
      if (mode == LevelMode::kDuration) {
        threshold *= 1000;
        thresholds.push_back(threshold);
        levels.push_back(std::to_string(get_int_any(v, 0)) + "s");
      } else {
        thresholds.push_back(threshold);
        levels.push_back(std::to_string(threshold));
      }
    }

    std::vector<size_t> idx(thresholds.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
      return thresholds[a] < thresholds[b];
    });
    std::vector<int> t_sorted;
    std::vector<std::string> l_sorted;
    t_sorted.reserve(thresholds.size());
    l_sorted.reserve(levels.size());
    for (auto i : idx) {
      t_sorted.push_back(thresholds[i]);
      l_sorted.push_back(levels[i]);
    }
    return {t_sorted, l_sorted};
  }

  // Iterate through all levels
  std::vector<std::pair<int, std::string>> combined;
  const auto keys = levelsNode.getMemberNames();
  for (auto& name : keys) {
    const Json::Value& lv = levelsNode[name];
    if (!lv.isObject()) continue;

    int threshold = get_int_any(lv["threshold"], 0);
    if (get_string(lv["unit"], "") == "seconds") {
      mode = LevelMode::kDuration;
      threshold *= 1000;
    } else {
      mode = LevelMode::kNumPeople;
    }
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
void send_json(evhttp_request* req, int code, const std::string& json) {
  evbuffer* out = evbuffer_new();
  if (!out) {
    evhttp_send_error(req, HTTP_SERVUNAVAIL, "No buffer");
    return;
  }
  LOGI("sending http code: {}, json: {}", code, json);
  evbuffer_add(out, json.data(), json.size());
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json; charset=utf-8");
  // (Optional) CORS
  evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");
  evhttp_send_reply(req, code, nullptr, out);
  evbuffer_free(out);
}

std::string escape_json(const std::string& s) {
  std::ostringstream os;
  for (unsigned char c : s) {
    switch (c) {
      case '\"': os << "\\\""; break;
      case '\\': os << "\\\\"; break;
      case '\b': os << "\\b"; break;
      case '\f': os << "\\f"; break;
      case '\n': os << "\\n"; break;
      case '\r': os << "\\r"; break;
      case '\t': os << "\\t"; break;
      default:
        if (c < 0x20) { os << "\\u" << std::hex << std::uppercase << (int)c; }
        else { os << c; }
    }
  }
  return os.str();
}

bool get_request_json(const evhttp_request* req, Json::Value& root) {
  char buf[kRecvBufMaxSize];
  ParsePostRequest(req, buf);

  const Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  JSONCPP_STRING errs;
  if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) &&
      errs.empty()) {
    return true;
  }
  return false;
}


void ApiSnapShot(evhttp_request* req, void*) {
  if (Json::Value json_req; get_request_json(req, json_req)) {
    std::string path;
    try {
      path = json_req["videoPath"].asString();
    }catch (const Json::Exception& e) {
      LOGE("failed to parse json: {}", e.what());
    }
    // Open video
    cv::VideoCapture cap;
    if (!cap.open(path)) {
      auto msg = std::format(R"({{"error":"failed to open video","path":"{}"}})", escape_json(path));
      send_json(req, HTTP_BADREQUEST, msg);
      return;
    }

    // Read first frame
    cv::Mat frame;
    if (!cap.read(frame) || frame.empty()) {
      auto msg = std::format(R"({{"error":"failed to read first frame","path":"{}" }})", escape_json(path));
      send_json(req, HTTP_BADREQUEST, msg);
      return;
    }

    // Encode to JPEG bytes
    std::vector<unsigned char> jpeg;
    if (std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
        !cv::imencode(".jpg", frame, jpeg, params)) {
      send_json(req, HTTP_SERVUNAVAIL, R"({"error":"JPEG encode failed"})");
      return;
    }

    // Base64 encode (cpp-base64: base64_encode(unsigned char const*, unsigned int, bool url=false))
    const std::string b64 = base64_encode(jpeg.data(),
                                          static_cast<unsigned int>(jpeg.size()),
                                          false /*url*/);

    // JSON response
    Json::Value json;
    json["mime"] = "image/jpeg";
    json["data"] = b64;

    send_json(req, HTTP_OK, json.toStyledString());
    LOGI("receive request for snapshot: {}. Successfully sent.", path);
  } else {
    LOGE("failed to parse json");
  }
}
void ApiIndex(evhttp_request* req, void*) {
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
void ApiHealth(evhttp_request* req, void*) {
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
        result_data_item["cameraId"] = monitor.video_code.data();
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
  result["msg"] = result_msg;  result["code"] = result_code;

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
    Monitor monitor;
    if (!root["code"].isString() || !root["cameraId"].isString()) {
      result_msg = "invalid request parameter";
    } else {
      std::string code = root["code"].asCString();
      std::string video_code = root["cameraId"].asCString();
      if (scheduler->ContainsMonitor(video_code, code)) {
        result_monitor["code"] = code;
        result_code = 1000;
        result_msg = "success";

      } else {
        result_msg = "the monitor does not exist";
      }
    }

  } else {
    result_msg = "invalid request parameter";
  }

  Json::Value result;
  result["control"] = result_monitor;
  result["msg"] = result_msg;
  result["code"] = result_code;

  LOGI("\n \t request:{} \n \t response:{}", root.toStyledString().data(),
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
  Json::Value result;

  if (reader->parse(buf, buf + std::strlen(buf), &root, &errs) &&
      errs.empty()) {

    // Required format:
    // { cameraId, sourceType, streamUrl, controls: [ {...}, {...} ] }
    if (!root.isObject() || !root.isMember("controls") || !root["controls"].isArray()) {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
      return;
    }

    const std::string camera_id = root["cameraId"].asString();
    const std::string source_type = root["sourceType"].asString();
    const std::string stream_url = root["streamUrl"].asString();
    const auto& controls = root["controls"];

    std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>> monitors;
    std::vector<std::string> control_codes;
    monitors.reserve(controls.size());
    control_codes.reserve(controls.size());

    for (const auto& ctrl : controls) {
      try {
        auto task = std::stoi(ctrl["algorithmCode"].asCString());
        if (task < 0 || task >= 6) {
          LOGE("wrong algorithm code: {}", task);
          continue;
        }

        const std::string ctrl_code = ctrl["code"].asString();
        control_codes.push_back(ctrl_code);

        LevelMode mode{};
        Monitor monitor{
            .code = ctrl_code,
            .video_code = camera_id,
            .source_type = source_type,
            .stream_url = stream_url,
            .push_stream = ctrl["pushStream"].asBool(),
            .push_stream_url = ctrl["pushStreamUrl"].asString(),
            .task = static_cast<Task>(task),
            .conf_threshold = ctrl["modelConfidenceThreshold"].asFloat(),
            .roi_str = ctrl["recognitionRegion"].asString(),
            .levels = parse_levels(ctrl["levels"], mode),
            .level_mode = mode,
        };

        const int ws = ctrl["alertWindowSize"].asInt();
        const int first_threshold = monitor.levels.first.empty() ? 0 : monitor.levels.first.front();
        const int64_t duration = (mode == LevelMode::kDuration) ? first_threshold : 0;

        EventDetectorConfig event_cfg{
            .major_stride_ms = static_cast<int64_t>(ctrl["minInterval"].asFloat() * 1000),
            .minor_stride_ms = static_cast<int64_t>(ctrl["alertInterval"].asFloat() * 1000),
            .window_size = ws,
            .min_vote = std::max(static_cast<int>(ws * ctrl["alertFrameRatio"].asFloat() / 100), 1),
            .max_event_duration_ms = static_cast<int64_t>(ctrl["maxAlarmDuration"].asFloat() * 1000),
            .latest_duration_ms = duration,
            .minimum_num = (mode == LevelMode::kNumPeople) ? std::max(first_threshold, 1) : 1,
        };

        std::string validate_msg;
        if (!monitor.ValidateAdd(validate_msg)) {
          LOGE("invalid monitor in controls: {}", validate_msg);
          continue;
        }

        monitors.emplace_back(std::make_unique<Monitor>(std::move(monitor)), std::move(event_cfg));
      } catch (const std::exception& e) {
        LOGE("runtime error in controls add: {}", e.what());
      }
    }

    std::vector<std::pair<int, std::string>> add_results;
    scheduler->MonitorBatchAdd(std::move(monitors), add_results);

    if (source_type == "stream") {
      Json::Value arr(Json::arrayValue);
      const size_t n = std::min(control_codes.size(), add_results.size());
      for (size_t i = 0; i < n; ++i) {
        Json::Value item;
        item["controlCode"] = control_codes[i];
        item["code"] = add_results[i].first;
        item["msg"] = add_results[i].second;
        arr.append(item);
      }
      send_json(req, HTTP_OK, arr.toStyledString());
      return;
    }

    // source_type == "file" : must be all successful or failed
    bool all_ok = !add_results.empty();
    std::string fail_msg;
    for (const auto& [rc, msg] : add_results) {
      if (rc != 1000) {
        all_ok = false;
        fail_msg = msg;
        break;
      }
    }
    Json::Value obj;
    obj["cameraId"] = camera_id;
    obj["code"] = all_ok ? 1000 : 0;
    obj["msg"] = all_ok ? "added successfully" : (fail_msg.empty() ? "batch add failed" : fail_msg);
    send_json(req, HTTP_OK, obj.toStyledString());
    return;
  } else {
    result_msg = "invalid request parameter";
    result["msg"] = result_msg;
    result["code"] = result_code;
  }

  LOGI("\n \t request:{} \n \t response:{}", root.toStyledString().data(),
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

  if (!(reader->parse(buf, buf + std::strlen(buf), &root, &errs) &&
        errs.empty())) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
    return;
  }

  // Required format:
  // { "cameraId": "...", "sourceType": "stream", "codes": ["..."] }
  if (!root.isObject() || !root.isMember("codes") || !root["codes"].isArray()) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
    return;
  }

  const std::string camera_id = root["cameraId"].asString();
  const std::string source_type = root["sourceType"].asString();
  const auto& codes = root["codes"];

  if (source_type == "file") {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"remove monitors for sourceType=file is not supported"})");
    return;
  }

  std::vector<std::pair<std::string, std::string>> monitor_ids;
  std::vector<std::string> control_codes;
  monitor_ids.reserve(codes.size());
  control_codes.reserve(codes.size());

  for (const auto& c : codes) {
    if (!c.isString()) {
      continue;
    }
    const std::string code = c.asString();
    if (code.empty()) {
      continue;
    }
    control_codes.push_back(code);
    monitor_ids.emplace_back(camera_id, code);
  }

  std::vector<std::pair<int, std::string>> rm_results;
  scheduler->MonitorBatchRemove(monitor_ids, rm_results);

  Json::Value arr(Json::arrayValue);
  const size_t n = std::min(control_codes.size(), rm_results.size());
  for (size_t i = 0; i < n; ++i) {
    Json::Value item;
    item["controlCode"] = control_codes[i];
    item["code"] = rm_results[i].first;
    item["msg"] = rm_results[i].second;
    arr.append(item);
  }
  send_json(req, HTTP_OK, arr.toStyledString());
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
void alarm_sound_cv(evhttp_request* req, void* arg) {
  auto* alarm_ptr = static_cast<AlarmController*>(arg);
  if (!alarm_ptr) {
    send_json(req, HTTP_SERVUNAVAIL, R"({\"msg\":\"alarm sound is not configured\"})");
    return;
  }

  // Parse GET query
  evkeyvalq params{};
  ParseGetRequest(req, &params);
  const char* method = evhttp_find_header(&params, "method");
  if (!method) {
    evhttp_clear_headers(&params);
    send_json(req, HTTP_BADREQUEST, R"({"msg":"missing parameter 'method'"})");
    return;
  }

  // Dispatch
  const std::string m(method);
  if (m == "stop") {
    alarm_ptr->stop();
    evhttp_clear_headers(&params);
    evhttp_send_reply(req, HTTP_OK, "OK", nullptr);
    return;
  } else if (m == "start") {
    alarm_ptr->trigger();
    evhttp_clear_headers(&params);
    evhttp_send_reply(req, HTTP_OK, "OK", nullptr);
    return;
  } else {
    evhttp_clear_headers(&params);
    send_json(req, HTTP_BADREQUEST, R"({"msg":"wrong method"})");
    return;
  }
}