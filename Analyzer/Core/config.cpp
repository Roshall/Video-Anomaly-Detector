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
    } catch (const Json::Exception& e) {
      LOGE("Json parse error:{}", e.what());
      return;
    }

    std::filesystem::path path(upload_dir);
    try {
      if (!std::filesystem::exists(path)) {
        std::filesystem::create_directory(path);
      }

      state = true;
    } catch (std::filesystem::filesystem_error& e) {
      std::cout << e.what() << std::endl;
    }

  } else {
    LOGE("parse {} error", file);
  }
  ifs.close();
}

void Config::Show() {
  printf("config.host={}\n", admin_host.c_str());
  printf("config.adminHost={}\n", admin_host.c_str());
  printf("config.analyzerHost={}\n", analyzer_host.c_str());
  printf("config.mediaHttpHost={}\n", media_http_host.c_str());
  printf("config.mediaRtspHost={}\n", media_rtsp_host.c_str());
  printf("config.uploadDir={}\n", upload_dir.data());
  printf("config.modelDir={}\n", model_dir.data());
}
}  // namespace avs_analyzer