#include "server_apis/api_common.h"

#include <algorithm>
#include <charconv>
#include <exception>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "Utils/common.h"
#include "Utils/log.h"
#include "monitor.h"
#include "scheduler.h"
#include "server_apis/server_context.h"
#include "task.h"

namespace {
bool to_int(const std::string& s, int& out) {
  auto* begin = s.data();
  auto* end = s.data() + s.size();
  auto rc = std::from_chars(begin, end, out);
  return rc.ec == std::errc() && rc.ptr == end;
}

int get_int_any(const nlohmann::json& v, int def = 0) {
  if (v.is_number_integer()) {
    return v.get<int>();
  }
  if (v.is_number_unsigned()) {
    return static_cast<int>(v.get<unsigned int>());
  }
  if (v.is_string()) {
    int x{};
    if (to_int(v.get<std::string>(), x)) {
      return x;
    }
  }
  if (v.is_number_float()) {
    return static_cast<int>(v.get<double>());
  }
  return def;
}

std::string get_string(const nlohmann::json& v, const std::string& def = "") {
  return v.is_string() ? v.get<std::string>() : def;
}

std::u8string to_u8string(const std::string& s) {
  return std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size());
}

std::pair<std::vector<int>, std::vector<std::string>> parse_levels(
    const nlohmann::json& levelsNode,
    avs_analyzer::LevelMode& mode) {
  std::vector<int> thresholds;
  std::vector<std::string> levels;

  if (levelsNode.is_null() || !levelsNode.is_object()) {
    return {thresholds, levels};
  }

  if (levelsNode.contains("values")) {
    const auto mode_str = get_string(levelsNode.value("mode", nlohmann::json{}), "");
    mode =  mode_str == "duration" ? avs_analyzer::LevelMode::kDuration
               : avs_analyzer::LevelMode::kNumPeople;

    const auto& values = levelsNode["values"];
    if (values.is_array()) {
      thresholds.reserve(values.size());
      levels.reserve(values.size());

      for (const auto& v : values) {
        int threshold = get_int_any(v, 0);
        if (mode == avs_analyzer::LevelMode::kDuration) {
          threshold *= 1000;
          thresholds.push_back(threshold);
          levels.push_back(std::to_string(get_int_any(v, 0)));
        } else {
          thresholds.push_back(threshold);
          levels.push_back(std::to_string(threshold));
        }
      }
    } else if (values.is_object()) {
      thresholds.reserve(values.size());
      levels.reserve(values.size());

      for (auto it = values.begin(); it != values.end(); ++it) {
        int threshold = get_int_any(it.value(), 0);
        if (mode == avs_analyzer::LevelMode::kDuration) {
          threshold *= 1000;
        }
        thresholds.push_back(threshold);
        levels.push_back(it.key());
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

  return {thresholds, levels};
}
}

void ApiAllMonitors(evhttp_request* req, void* arg) {
  auto scheduler = static_cast<avs_analyzer::Scheduler*>(arg);

  int result_code = 0;
  std::string result_msg = "error";

  nlohmann::json result;
  if (auto monitors = scheduler->GetAllMonitors(); !monitors.empty()) {
    nlohmann::json result_data = nlohmann::json::array();
    int64_t curTimestamp = avs_analyzer::getCurTimestamp();
    for (auto& monitor : monitors) {
      const int64_t startTimestamp = monitor.start_timestamp;
      result_data.push_back({
          {"code", monitor.code},
          {"cameraId", monitor.video_code},
          {"streamUrl", monitor.stream_url},
          {"pushStream", monitor.push_stream},
          {"pushStreamUrl", monitor.push_stream_url},
          {"algorithmCode", toString(monitor.task)},
          {"recognitionRegion", monitor.roi_str},
          {"sourceType", monitor.source_type},
          {"startTimestamp", startTimestamp},
          {"liveMilliseconds", curTimestamp - startTimestamp}});
    }
    result["data"] = result_data;
    result_code = 1000;
    result_msg = "success";
  } else {
    result_msg = "the number of control exector is empty";
  }
  result["msg"] = result_msg;
  result["code"] = result_code;

  send_json(req, HTTP_OK, result.dump());
}

void ApiMonitor(evhttp_request* req, void* arg) {
  auto scheduler = static_cast<avs_analyzer::Scheduler*>(arg);

  nlohmann::json result_monitor;
  int result_code = 0;
  std::string result_msg = "error";

  nlohmann::json root;
  if (get_request_json(req, root)) {
    if (!root.contains("code") || !root["code"].is_string() ||
        !root.contains("cameraId") || !root["cameraId"].is_string()) {
      result_msg = "invalid request parameter";
    } else {
      std::string code = root["code"].get<std::string>();
      std::string video_code = root["cameraId"].get<std::string>();
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

  nlohmann::json result;
  result["control"] = result_monitor;
  result["msg"] = result_msg;
  result["code"] = result_code;

  LOGI("\n \t request:{} \n \t response:{}", root.dump(), result.dump());
  send_json(req, HTTP_OK, result.dump());
}

void ApiAddMonitor(evhttp_request* req, void* arg) {
  auto* ctx = static_cast<ServerContext*>(arg);
  if (!ctx || !ctx->scheduler || !ctx->base) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"msg":"server is shutting down"})");
    return;
  }
  avs_analyzer::Scheduler* scheduler = ctx->scheduler;

  int result_code = 0;
  std::string result_msg;

  nlohmann::json result;
  nlohmann::json root;
  if (get_request_json(req, root)) {
    if (!root.is_object() || !root.contains("controls") || !root["controls"].is_array()) {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
      return;
    }
    LOGD("Received AddMonitor request: {}", root.dump());

    const std::string camera_id = root.value("cameraId", "");
    const std::string source_type = root.value("sourceType", "");
    const std::string stream_url = root.value("streamUrl", "");

    if (camera_id.empty()) {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"cameraId is required and cannot be empty"})");
      return;
    }

    if (stream_url.empty()) {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"streamUrl is required and cannot be empty"})");
      return;
    }

    if (source_type != "stream" && source_type != "file") {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"sourceType must be 'stream' or 'file'"})");
      return;
    }

    const auto& controls = root["controls"];
    if (controls.empty()) {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"controls array cannot be empty"})");
      return;
    }

    std::vector<std::pair<std::unique_ptr<avs_analyzer::Monitor>, avs_analyzer::EventDetectorConfig>> monitors;
    std::vector<std::string> control_codes;
    monitors.reserve(controls.size());
    control_codes.reserve(controls.size());

    for (const auto& ctrl : controls) {
      try {
        if (!ctrl.is_object()) {
          LOGE("control item is not an object, skipping");
          continue;
        }

        int task = 0;
        if (ctrl.contains("algorithmCode")) {
          const auto& ac = ctrl["algorithmCode"];
          if (ac.is_string()) {
            try {
              task = std::stoi(ac.get<std::string>());
            } catch (const std::exception&) {
              LOGE("invalid algorithmCode string, skipping control");
              continue;
            }
          } else if (ac.is_number_integer()) {
            task = ac.get<int>();
          } else {
            LOGE("algorithmCode has invalid type, skipping control");
            continue;
          }
        } else {
          LOGE("algorithmCode is missing, skipping control");
          continue;
        }

        if (task < 0 || task >= 6) {
          LOGE("wrong algorithm code: {}, must be in range [0, 5]", task);
          continue;
        }

        const std::string ctrl_code = ctrl.value("code", "");
        if (ctrl_code.empty()) {
          LOGE("control code is empty, skipping control");
          continue;
        }

        const std::string behavior = ctrl.value("behavior", "");

        const bool push_stream = ctrl.value("pushStream", false);
        const std::string push_stream_url = ctrl.value("pushStreamUrl", "");

        if (push_stream && push_stream_url.empty()) {
          LOGE("pushStream is true but pushStreamUrl is empty for control {}, skipping", ctrl_code);
          continue;
        }

        const float conf_threshold = ctrl.value("modelConfidenceThreshold", 0.5f);
        if (conf_threshold < 0.0f || conf_threshold > 1.0f) {
          LOGE("modelConfidenceThreshold {} is out of range [0.0, 1.0] for control {}, using default 0.5", 
               conf_threshold, ctrl_code);
        }

        std::string roi_str;
        if (ctrl.contains("recognitionRegion")) {
          const auto& rr = ctrl["recognitionRegion"];
          if (rr.is_string()) {
            roi_str = rr.get<std::string>();
          } else if (rr.is_array()) {
            roi_str = rr.dump();
          }
        }
        if (roi_str.empty()) {
          LOGE("recognitionRegion is empty for control {}, skipping", ctrl_code);
          continue;
        }

        control_codes.push_back(ctrl_code);

        avs_analyzer::LevelMode mode{};
        avs_analyzer::Monitor monitor{
            .code = ctrl_code,
            .video_code = camera_id,
            .source_type = source_type,
            .stream_url = stream_url,
            .push_stream = push_stream,
            .push_stream_url = push_stream_url,
            .task = static_cast<avs_analyzer::Task>(task),
            .conf_threshold = std::max(0.0f, std::min(1.0f, conf_threshold)),
            .behavior = to_u8string(behavior),
            .roi_str = roi_str,
            .levels = parse_levels(ctrl.contains("levels") ? ctrl["levels"] : nlohmann::json{}, mode),
            .level_mode = mode,
        };

        const int ws = ctrl.value("alertWindowSize", 1);
        if (ws <= 0) {
          LOGE("alertWindowSize {} is invalid for control {}, using default 1", ws, ctrl_code);
        }
        const int window_size = std::max(1, ws);

        const float alert_frame_ratio = ctrl.value("alertFrameRatio", 60.0f);
        if (alert_frame_ratio < 0.0f || alert_frame_ratio > 100.0f) {
          LOGE("alertFrameRatio {} is out of range [0.0, 100.0] for control {}, using default 60.0",
               alert_frame_ratio, ctrl_code);
        }
        const float safe_ratio = std::max(0.0f, std::min(100.0f, alert_frame_ratio));

        const float min_interval = ctrl.value("minInterval", 2.0f);
        if (min_interval < 0.0f) {
          LOGE("minInterval {} is negative for control {}, using default 2.0", min_interval, ctrl_code);
        }

        const float alert_interval = ctrl.value("alertInterval", 0.5f);
        if (alert_interval < 0.0f) {
          LOGE("alertInterval {} is negative for control {}, using default 0.5", alert_interval, ctrl_code);
        }

        const float max_alarm_duration = ctrl.value("maxAlarmDuration", 60.0f);
        if (max_alarm_duration < 0.0f) {
          LOGE("maxAlarmDuration {} is negative for control {}, using default 60.0", 
               max_alarm_duration, ctrl_code);
        }

        const int first_threshold = monitor.levels.first.empty() ? 0 : monitor.levels.first.front();
        const int64_t duration = (mode == avs_analyzer::LevelMode::kDuration) ? first_threshold : 0;

        avs_analyzer::EventDetectorConfig event_cfg{
            .major_stride_ms = static_cast<int64_t>(std::max(0.0f, min_interval) * 1000),
            .minor_stride_ms = static_cast<int64_t>(std::max(0.0f, alert_interval) * 1000),
            .window_size = window_size,
            .min_vote = std::max(1, static_cast<int>(window_size * safe_ratio / 100.0f)),
            .max_event_duration_ms = static_cast<int64_t>(std::max(0.0f, max_alarm_duration) * 1000),
            .latest_duration_ms = duration,
            .minimum_num = (mode == avs_analyzer::LevelMode::kNumPeople) ? std::max(first_threshold, 1) : 1,
        };

        const std::string thresholds_log = monitor.levels.first.empty() ? "none" : std::to_string(monitor.levels.first[0]);
        const std::string levels_log = monitor.levels.second.empty() ? "none" : monitor.levels.second[0];
        LOGD("Parsed monitor config: code={}, task={}, thresholds={}, levels={}, event_cfg={{major_stride_ms: {}, minor_stride_ms: {}, window_size: {}, min_vote: {}, max_event_duration_ms: {}, latest_duration_ms: {}, minimum_num: {}}}",
             monitor.code, toString(monitor.task), thresholds_log, levels_log,
             event_cfg.major_stride_ms, event_cfg.minor_stride_ms, event_cfg.window_size, event_cfg.min_vote,
             event_cfg.max_event_duration_ms, event_cfg.latest_duration_ms, event_cfg.minimum_num);

        std::string validate_msg;
        if (!monitor.ValidateAdd(validate_msg)) {
          LOGE("invalid monitor in controls: {}", validate_msg);
          continue;
        }

        monitors.emplace_back(std::make_unique<avs_analyzer::Monitor>(std::move(monitor)), std::move(event_cfg));
      } catch (const std::exception& e) {
        LOGE("runtime error in controls add: {}", e.what());
      }
    }

    if (monitors.empty()) {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"no valid monitors to add after validation"})");
      return;
    }

    // Parsing and validation stay on the event loop; the blocking scheduler
    // call (RTSP connect / model load) runs on the control pool and the reply
    // is delivered back on the loop.
    evhttp_request_own(req);
    avs_analyzer::g_control_pool.detach_task(
        [scheduler, base = ctx->base, req,
         monitors = std::move(monitors),
         control_codes = std::move(control_codes), camera_id,
         is_stream = source_type == "stream"]() mutable {
          std::vector<std::pair<int, std::string>> add_results;
          scheduler->MonitorBatchAdd(std::move(monitors), add_results);

          std::string body;
          if (is_stream) {
            nlohmann::json arr = nlohmann::json::array();
            const size_t n =
                std::min(control_codes.size(), add_results.size());
            for (size_t i = 0; i < n; ++i) {
              arr.push_back({
                  {"controlCode", control_codes[i]},
                  {"code", add_results[i].first},
                  {"msg", add_results[i].second}});
            }
            body = arr.dump();
          } else {
            bool all_ok = !add_results.empty();
            std::string fail_msg;
            for (const auto& [rc, msg] : add_results) {
              if (rc != 1000) {
                all_ok = false;
                fail_msg = msg;
                break;
              }
            }
            nlohmann::json obj{
                {"cameraId", camera_id},
                {"code", all_ok ? 1000 : 0},
                {"msg", all_ok ? "added successfully"
                               : (fail_msg.empty() ? "batch add failed"
                                                   : fail_msg)}};
            body = obj.dump();
          }
          ReplyJsonAsync(base, req, HTTP_OK, std::move(body));
        });
    return;
  } else {
    result_msg = "invalid request parameter";
    result["msg"] = result_msg;
    result["code"] = result_code;
  }

  LOGI("\n \t request:{} \n \t response:{}", root.dump(), result.dump());
  send_json(req, HTTP_OK, result.dump());
}

void ApiRemoveMonitor(evhttp_request* req, void* arg) {
  auto* ctx = static_cast<ServerContext*>(arg);
  if (!ctx || !ctx->scheduler || !ctx->base) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"msg":"server is shutting down"})");
    return;
  }
  avs_analyzer::Scheduler* scheduler = ctx->scheduler;

  nlohmann::json root;
  if (!get_request_json(req, root)) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
    return;
  }

  if (!root.is_object() || !root.contains("codes") || !root["codes"].is_array()) {
    send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
    return;
  }

  const std::string camera_id = root.value("cameraId", "");
  const std::string source_type = root.value("sourceType", "");
  const auto& codes = root["codes"];

  if (source_type == "file") {
    send_json(req, HTTP_BADREQUEST,
              R"({"msg":"remove monitors for sourceType=file is not supported"})");
    return;
  }

  std::vector<std::pair<std::string, std::string>> monitor_ids;
  std::vector<std::string> control_codes;
  monitor_ids.reserve(codes.size());
  control_codes.reserve(codes.size());

  for (const auto& c : codes) {
    if (!c.is_string()) {
      continue;
    }
    const std::string code = c.get<std::string>();
    if (code.empty()) {
      continue;
    }
    control_codes.push_back(code);
    monitor_ids.emplace_back(camera_id, code);
  }

  evhttp_request_own(req);
  avs_analyzer::g_control_pool.detach_task(
      [scheduler, base = ctx->base, req,
       monitor_ids = std::move(monitor_ids),
       control_codes = std::move(control_codes)]() mutable {
        std::vector<std::pair<int, std::string>> rm_results;
        scheduler->MonitorBatchRemove(monitor_ids, rm_results);

        nlohmann::json arr = nlohmann::json::array();
        const size_t n = std::min(control_codes.size(), rm_results.size());
        for (size_t i = 0; i < n; ++i) {
          arr.push_back({
              {"controlCode", control_codes[i]},
              {"code", rm_results[i].first},
              {"msg", rm_results[i].second}});
        }
        ReplyJsonAsync(base, req, HTTP_OK, arr.dump());
      });
}
