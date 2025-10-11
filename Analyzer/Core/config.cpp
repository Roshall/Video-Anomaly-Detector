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
    LOGE("open %s error", file);
    return;
  }
  Json::CharReaderBuilder builder;
  builder["collectComments"] = true;
  JSONCPP_STRING errs;
  Json::Value root;

  if (parseFromStream(builder, ifs, &root, &errs)) {
    admin_host = std::format("http://{}", root["adminHost"].asString());
    analyzer_host = std::format("http://{}", root["analyzerHost"].asInt());
    media_http_host = std::format("http://{}", root["mediaHttpHost"].asInt());
    media_rtsp_host = std::format("http://{}", root["mediaRtspHost"].asInt());

    upload_dir = root["uploadDir"].asString();
    model_dir = root["modelDir"].asString();

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
    LOGE("parse %s error", file);
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