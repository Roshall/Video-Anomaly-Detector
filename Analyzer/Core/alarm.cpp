#include "generate_alarm_video.h"

#include <json/json.h>

#include <filesystem>
#include <iostream>
#include <opencv2/opencv.hpp>

#include "Utils/common.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "config.h"
#include "frame_recorder.h"
extern "C" {
//#include "libswscale/swscale.h"
#include <libavutil/imgutils.h>
//#include <libswresample/swresample.h>
}


namespace avs_analyzer {

Alarm::Alarm(int64_t happenTimestamp, std::string_view controlCode, Event event,
             const std::shared_ptr<Recorder>& recorder)
    : start_ts(happenTimestamp),
      control_code(controlCode),
      recorder(recorder),
      event(std::move(event)) {}
Alarm::~Alarm() {}
bool Alarm::Finalize(Config& config) {
  if (!recorder->dst().empty()) {
    std::string prifixDir = config.upload_dir + "/alarm";

    std::filesystem::path prefix_path1(prifixDir);
    try {
      if (!std::filesystem::exists(prefix_path1)) {
        std::filesystem::create_directory(prefix_path1);
      }
    } catch (std::filesystem::filesystem_error& e) {
      std::cout << "genAlarmVideo() create_directory1 error:" << e.what()
                << std::endl;
      return false;
    }

    prifixDir = prifixDir + "/" + control_code;

    std::filesystem::path prefix_path2(prifixDir);
    try {
      if (!std::filesystem::exists(prefix_path2)) {
        std::filesystem::create_directory(prefix_path2);
      }
    } catch (std::filesystem::filesystem_error& e) {
      std::cout << "genAlarmVideo() create_directory2 error:" << e.what()
                << std::endl;
      return false;
    }
    std::string ymdhms_rd = std::format("{}_{}", start_ts, getRandomInt());
    prifixDir += "/" + ymdhms_rd;
    std::filesystem::path prefix_dir_path3(prifixDir);
    try {
      if (!std::filesystem::exists(prefix_dir_path3)) {
        std::filesystem::create_directory(prefix_dir_path3);
      }
    } catch (std::filesystem::filesystem_error& e) {
      std::cout << "genAlarmVideo() create_directory3 error:" << e.what()
                << std::endl;
      return false;
    }

    std::string video_path = "alarm/" + control_code + "/" + ymdhms_rd +
                             "/main.ts";  // 视频相对路径
//    std::string image_path = "alarm/" + control_code + "/" + ymdhms_rd +
//                             "/main.jpg";  // 图片相对路径

    std::string video_path_abs = config.upload_dir + "/" + video_path;
//    std::string image_path_abs = config.upload_dir + "/" + image_path;

    if (!recorder->Initialize(video_path_abs.data())) {
      std::cout << "genAlarm can not initialize recorder error" << std::endl;
      return false;
    }

    // 上传报警信息start
    std::string url = config.admin_host + "/api/postAddAlarm";
    Json::Value param;
    param["control_code"] = control_code;
    param["desc"] = "";
    param["video_path"] = video_path;
//    param["image_path"] = image_path;

    std::string data = param.toStyledString();
    Request request;
    std::string response;
    request.post(url.data(), data.data(), response);

    LOGI("\n \t request:%s \n \t response:%s", url.data(), response.data());
  }
  for (auto& frame : event.frames) {
    recorder->AddVideoFrame(frame.get());
    // TODO: create cover image
  }
  return true;
}
}