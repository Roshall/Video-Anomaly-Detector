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
    } catch (const Json::Exception& e) {
      LOGE("Json parse error:{}", e.what());
      return;
    }

    // 0: use all core
    thread_pool_size = std::clamp(thread_pool_size, 0, static_cast<int>(std::thread::hardware_concurrency()));

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
  printf("config.host=%s\n", admin_host.c_str());
  printf("config.adminHost=%s\n", admin_host.c_str());
  printf("config.analyzerHost=%s\n", analyzer_host.c_str());
  printf("config.mediaHttpHost=%s\n", media_http_host.c_str());
  printf("config.mediaRtspHost=%s\n", media_rtsp_host.c_str());
  printf("config.uploadDir=%s\n", upload_dir.data());
  printf("config.modelDir=%s\n", model_dir.data());
}
}  // namespace avs_analyzer