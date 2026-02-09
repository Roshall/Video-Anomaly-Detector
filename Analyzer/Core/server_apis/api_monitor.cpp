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

std::pair<std::vector<int>, std::vector<std::string>> parse_levels(
    const nlohmann::json& levelsNode,
    avs_analyzer::LevelMode& mode) {
  std::vector<int> thresholds;
  std::vector<std::string> levels;

  if (levelsNode.is_null() || !levelsNode.is_object()) {
    return {thresholds, levels};
  }

  if (levelsNode.contains("values") && levelsNode["values"].is_array()) {
    const auto mode_str = get_string(levelsNode.value("mode", nlohmann::json{}), "");
    mode = (mode_str == "seconds" || mode_str == "duration")
               ? avs_analyzer::LevelMode::kDuration
               : avs_analyzer::LevelMode::kNumPeople;

    const auto& values = levelsNode["values"];
    thresholds.reserve(values.size());
    levels.reserve(values.size());

    for (const auto& v : values) {
      int threshold = get_int_any(v, 0);
      if (mode == avs_analyzer::LevelMode::kDuration) {
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

  std::vector<std::pair<int, std::string>> combined;
  for (auto it = levelsNode.begin(); it != levelsNode.end(); ++it) {
    const auto& name = it.key();
    const auto& lv = it.value();
    if (!lv.is_object()) {
      continue;
    }

    int threshold = get_int_any(lv.value("threshold", nlohmann::json{}), 0);
    if (get_string(lv.value("unit", nlohmann::json{}), "") == "seconds") {
      mode = avs_analyzer::LevelMode::kDuration;
      threshold *= 1000;
    } else {
      mode = avs_analyzer::LevelMode::kNumPeople;
    }
    combined.emplace_back(threshold, name);
  }

  std::sort(combined.begin(), combined.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });
  for (const auto& [threshold, unit] : combined) {
    thresholds.push_back(threshold);
    levels.push_back(unit);
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
  const auto scheduler = static_cast<avs_analyzer::Scheduler*>(arg);

  int result_code = 0;
  std::string result_msg;

  nlohmann::json result;
  nlohmann::json root;
  if (get_request_json(req, root)) {
    if (!root.is_object() || !root.contains("controls") || !root["controls"].is_array()) {
      send_json(req, HTTP_BADREQUEST, R"({"msg":"invalid request parameter"})");
      return;
    }

    const std::string camera_id = root.value("cameraId", "");
    const std::string source_type = root.value("sourceType", "");
    const std::string stream_url = root.value("streamUrl", "");
    const auto& controls = root["controls"];

    std::vector<std::pair<std::unique_ptr<avs_analyzer::Monitor>, avs_analyzer::EventDetectorConfig>> monitors;
    std::vector<std::string> control_codes;
    monitors.reserve(controls.size());
    control_codes.reserve(controls.size());

    for (const auto& ctrl : controls) {
      try {
        int task = 0;
        if (ctrl.contains("algorithmCode")) {
          const auto& ac = ctrl["algorithmCode"];
          if (ac.is_string()) {
            task = std::stoi(ac.get<std::string>());
          } else if (ac.is_number_integer()) {
            task = ac.get<int>();
          } else {
            task = 0;
          }
        }
        if (task < 0 || task >= 6) {
          LOGE("wrong algorithm code: {}", task);
          continue;
        }

        const std::string ctrl_code = ctrl.value("code", "");
        control_codes.push_back(ctrl_code);

        avs_analyzer::LevelMode mode{};
        avs_analyzer::Monitor monitor{
            .code = ctrl_code,
            .video_code = camera_id,
            .source_type = source_type,
            .stream_url = stream_url,
            .push_stream = ctrl.value("pushStream", false),
            .push_stream_url = ctrl.value("pushStreamUrl", ""),
            .task = static_cast<avs_analyzer::Task>(task),
            .conf_threshold = ctrl.value("modelConfidenceThreshold", 0.5f),
            .roi_str = ctrl.value("recognitionRegion", ""),
            .levels = parse_levels(ctrl.contains("levels") ? ctrl["levels"] : nlohmann::json{}, mode),
            .level_mode = mode,
        };

        const int ws = ctrl.value("alertWindowSize", 1);
        const int first_threshold = monitor.levels.first.empty() ? 0 : monitor.levels.first.front();
        const int64_t duration = (mode == avs_analyzer::LevelMode::kDuration) ? first_threshold : 0;

        avs_analyzer::EventDetectorConfig event_cfg{
            .major_stride_ms = static_cast<int64_t>(ctrl.value("minInterval", 0.0f) * 1000),
            .minor_stride_ms = static_cast<int64_t>(ctrl.value("alertInterval", 0.0f) * 1000),
            .window_size = ws,
            .min_vote = std::max(static_cast<int>(ws * ctrl.value("alertFrameRatio", 100.0f) / 100), 1),
            .max_event_duration_ms = static_cast<int64_t>(ctrl.value("maxAlarmDuration", 0.0f) * 1000),
            .latest_duration_ms = duration,
            .minimum_num = (mode == avs_analyzer::LevelMode::kNumPeople) ? std::max(first_threshold, 1) : 1,
        };

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

    std::vector<std::pair<int, std::string>> add_results;
    scheduler->MonitorBatchAdd(std::move(monitors), add_results);

    if (source_type == "stream") {
      nlohmann::json arr = nlohmann::json::array();
      const size_t n = std::min(control_codes.size(), add_results.size());
      for (size_t i = 0; i < n; ++i) {
        arr.push_back({
            {"controlCode", control_codes[i]},
            {"code", add_results[i].first},
            {"msg", add_results[i].second}});
      }
      send_json(req, HTTP_OK, arr.dump());
      return;
    }

    bool all_ok = !add_results.empty();
    std::string fail_msg;
    for (const auto& [rc, msg] : add_results) {
      if (rc != 1000) {
        all_ok = false;
        fail_msg = msg;
        break;
      }
    }
    nlohmann::json obj{{"cameraId", camera_id},
                       {"code", all_ok ? 1000 : 0},
                       {"msg", all_ok ? "added successfully"
                                      : (fail_msg.empty() ? "batch add failed" : fail_msg)}};
    send_json(req, HTTP_OK, obj.dump());
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
  auto scheduler = static_cast<avs_analyzer::Scheduler*>(arg);

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
  send_json(req, HTTP_OK, arr.dump());
}
