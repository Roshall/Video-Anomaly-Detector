#include "config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "Utils/log.h"
#include <format>
using namespace std::string_literals;

namespace avs_analyzer {
namespace {
template <typename T>
T json_get_or(const nlohmann::json& j, std::initializer_list<const char*> path,
              T def) {
  const nlohmann::json* cur = &j;
  for (const char* key : path) {
    if (!cur->is_object() || !cur->contains(key)) {
      return def;
    }
    cur = &(*cur)[key];
  }
  try {
    return cur->get<T>();
  } catch (...) {
    return def;
  }
}

std::string json_get_or_string(const nlohmann::json& j,
                               std::initializer_list<const char*> path,
                               std::string def = {}) {
  return json_get_or<std::string>(j, path, std::move(def));
}
}  // namespace

Config::Config(const char* file) {
  std::ifstream ifs(file, std::ios::binary);

  if (!ifs.is_open()) {
    LOGE("open {} error", file);
    return;
  }
  nlohmann::json root;
  try {
    ifs >> root;
  } catch (const std::exception& e) {
    LOGE("Json parse error:{}", e.what());
    return;
  }

  try {
    // Prefer new hierarchical schema; fall back to legacy flat keys.
    const std::string admin_host_raw =
        json_get_or_string(root, {"server", "admin", "host"},
                           json_get_or_string(root, {"adminHost"}, ""));
    const std::string analyzer_host_raw =
        json_get_or_string(root, {"server", "analyzer", "host"},
                           json_get_or_string(root, {"analyzerHost"}, ""));
    const std::string media_http_raw =
        json_get_or_string(root, {"server", "media", "http"},
                           json_get_or_string(root, {"mediaHttpHost"}, ""));
    const std::string media_rtsp_raw =
        json_get_or_string(root, {"server", "media", "rtsp"},
                           json_get_or_string(root, {"mediaRtspHost"}, ""));

    if (!admin_host_raw.empty()) {
      admin_host = std::format("http://{}", admin_host_raw);
    }
    if (!analyzer_host_raw.empty()) {
      analyzer_host = std::format("http://{}", analyzer_host_raw);
    }
    if (!media_http_raw.empty()) {
      media_http_host = std::format("http://{}", media_http_raw);
    }
    if (!media_rtsp_raw.empty()) {
      // NOTE: kept as http:// for backward compatibility with existing code paths.
      media_rtsp_host = std::format("http://{}", media_rtsp_raw);
    }

    upload_dir = json_get_or_string(root, {"uploadDir"}, upload_dir);
    model_dir = json_get_or_string(root, {"DLModel", "dir"},
                                   json_get_or_string(root, {"modelDir"}, model_dir));

    use_accelerator = json_get_or<bool>(
        root, {"DLModel", "acceleratorInference"},
        json_get_or<bool>(root, {"application", "acceleratorInference"},
                          json_get_or<bool>(root, {"acceleratorInference"}, use_accelerator)));
    log_level = json_get_or_string(root, {"application", "logLevel"},
                                   json_get_or_string(root, {"logLevel"}, log_level));
    thread_pool_size = json_get_or<int>(root, {"application", "threadPoolSize"},
                                        json_get_or<int>(root, {"threadPoolSize"}, thread_pool_size));
    file_cache_timeout = json_get_or<u_int32_t>(
        root, {"application", "fileCacheTimeout"},
        json_get_or<u_int32_t>(root, {"fileCacheTimeout"}, file_cache_timeout));

    alarm_sound_path = json_get_or_string(
        root, {"alarm", "soundPath"},
        json_get_or_string(root, {"alarmSoundPath"}, alarm_sound_path));
    alarm_sound_cooldown_sec = json_get_or<int>(
        root, {"alarm", "soundCooldownSec"},
        json_get_or<int>(root, {"alarmSoundCooldownSec"}, alarm_sound_cooldown_sec));

    font_path = json_get_or_string(root, {"display", "fontPath"},
                                   json_get_or_string(root, {"fontPath"}, font_path));
    legend_position = json_get_or_string(
        root, {"display", "legendPosition"},
        json_get_or_string(root, {"legendPosition"}, legend_position));

    bunkoff_overlap_ratio = json_get_or<double>(
        root, {"analyzer", "bunkOffOverlapRatio"},
        json_get_or<double>(root, {"bunkOffOverlapRatio"}, bunkoff_overlap_ratio));
  } catch (const std::exception& e) {
    LOGE("Json parse error:{}", e.what());
    return;
  }

    // 0: use all core
    thread_pool_size = std::clamp(thread_pool_size, 0, static_cast<int>(std::thread::hardware_concurrency()));
    alarm_sound_cooldown_sec = std::max(0, alarm_sound_cooldown_sec);
    bunkoff_overlap_ratio = std::clamp(bunkoff_overlap_ratio, 0.0, 1.0);

    std::filesystem::path path(upload_dir);
    try {
      if (!std::filesystem::exists(path)) {
        std::filesystem::create_directory(path);
      }

      state = true;
    } catch (std::filesystem::filesystem_error& e) {
      LOGE("could not find upload dir: {}",e.what());
    }
  ifs.close();
}

void Config::Show() {
  printf("Configuration Settings:\n"
         "----------------------\n"
         "State               : %s\n"
         "Thread Pool Size    : %d\n"
         "File Cache Timeout  : %u seconds\n"
         "Admin Host         : %s\n"
         "Analyzer Host      : %s\n"
         "Media HTTP Host    : %s\n"
         "Media RTSP Host    : %s\n"
         "Upload Directory   : %s\n"
         "Model Directory    : %s\n"
         "Use Accelerator    : %s\n"
         "Log Level          : %s\n"
         "Alarm Sound Path   : %s\n"
         "Alarm Cooldown Sec : %d\n"
         "Font Path          : %s\n"
         "Legend Position    : %s\n"
         "BunkOff Overlap    : %.3f\n"
         "----------------------\n",
         state ? "true" : "false",
         thread_pool_size,
         file_cache_timeout,
         admin_host.c_str(),
         analyzer_host.c_str(),
         media_http_host.c_str(),
         media_rtsp_host.c_str(),
         upload_dir.data(),
         model_dir.data(),
         use_accelerator ? "true" : "false",
         log_level.c_str(),
         alarm_sound_path.c_str(),
         alarm_sound_cooldown_sec,
         font_path.c_str(),
         legend_position.c_str(),
         bunkoff_overlap_ratio);
}
}  // namespace avs_analyzer