#include "config.h"

#include <json/json.h>

#include <filesystem>
#include <fstream>
#include <iostream>

#include "Utils/log.h"
#include <format>
using namespace std::string_literals;

namespace avs_analyzer {
Config::Config(const char* file) {
  std::ifstream ifs(file, std::ios::binary);

  if (!ifs.is_open()) {
    LOGE("open {} error", file);
    return;
  }
  Json::CharReaderBuilder builder;
  builder["collectComments"] = true;
  JSONCPP_STRING errs;
  Json::Value root;

  if (parseFromStream(builder, ifs, &root, &errs)) {
    try {
      admin_host = std::format("http://{}", root["adminHost"].asString());
      analyzer_host = std::format("http://{}", root["analyzerHost"].asString());
      media_http_host = std::format("http://{}", root["mediaHttpHost"].asString());
      media_rtsp_host = std::format("http://{}", root["mediaRtspHost"].asString());

      upload_dir = root["uploadDir"].asCString();
      model_dir = root["modelDir"].asCString();
      use_accelerator = root["acceleratorInference"].asBool();
      log_level = root["logLevel"].asCString();
      thread_pool_size = root["threadPoolSize"].asInt();
      file_cache_timeout = root["fileCacheTimeout"].asUInt();

      if (root.isMember("alarmSoundPath") && root["alarmSoundPath"].isString()) {
        alarm_sound_path = root["alarmSoundPath"].asString();
      }
      if (root.isMember("alarmSoundCooldownSec")) {
        alarm_sound_cooldown_sec = root["alarmSoundCooldownSec"].asInt();
      }

      if (root.isMember("fontPath") && root["fontPath"].isString()) {
        font_path = root["fontPath"].asString();
      }
      if (root.isMember("legendPosition") && root["legendPosition"].isString()) {
        legend_position = root["legendPosition"].asString();
      }
    } catch (const Json::Exception& e) {
      LOGE("Json parse error:{}", e.what());
      return;
    }

    // 0: use all core
    thread_pool_size = std::clamp(thread_pool_size, 0, static_cast<int>(std::thread::hardware_concurrency()));
    alarm_sound_cooldown_sec = std::max(0, alarm_sound_cooldown_sec);

    std::filesystem::path path(upload_dir);
    try {
      if (!std::filesystem::exists(path)) {
        std::filesystem::create_directory(path);
      }

      state = true;
    } catch (std::filesystem::filesystem_error& e) {
      LOGE("could not find upload dir: {}",e.what());
    }

  } else {
    LOGE("parse {} error", file);
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
         legend_position.c_str());
}
}  // namespace avs_analyzer