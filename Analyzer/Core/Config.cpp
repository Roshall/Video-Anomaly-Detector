

#include "Config.h"

#include <json/json.h>

#include <filesystem>
#include <fstream>
#include <iostream>

#include "Utils/Log.h"

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
    host = root["host"].asString();
    admin_port = root["adminPort"].asInt();
    admin_host =
        "http://"s + this->host + ":"s + std::to_string(this->admin_port);
    analyzer_port = root["analyzerPort"].asInt();
    media_http_port = root["mediaHttpPort"].asInt();
    media_rtsp_port = root["mediaRtspPort"].asInt();

    upload_dir = root["uploadDir"].asString();
    model_dir = root["modelDir"].asString();

    algorithm_api_url = root["algorithmApiUrl"].asString();
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
  printf("config.host=%s\n", host.data());
  printf("config.adminPort=%d\n", admin_port);
  printf("config.analyzerPort=%d\n", analyzer_port);
  printf("config.mediaHttpPort=%d\n", media_http_port);
  printf("config.mediaRtspPort=%d\n", media_rtsp_port);

  printf("config.uploadDir=%s\n", upload_dir.data());
  printf("config.modelDir=%s\n", model_dir.data());
  printf("config.algorithmApiUrl=%s\n", algorithm_api_url.data());
}
}  // namespace avs_analyzer