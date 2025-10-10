#include "alarm.h"

#include <json/json.h>

#include <filesystem>
#include <iostream>
#include <opencv2/opencv.hpp>

#include "Utils/common.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "config.h"
#include "frame_recorder.h"
#include "frame_walk_time.h"
extern "C" {
// #include "libswscale/swscale.h"
#include <libavutil/imgutils.h>
// #include <libswresample/swresample.h>
}

namespace avs_analyzer {

Alarm::Alarm(std::chrono::time_point<std::chrono::system_clock> happenTimestamp,
             std::string_view controlCode, std::unique_ptr<Event> event,
             int width, int height, int fps)
    : start_ts(happenTimestamp),
      control_code(controlCode),
      event(std::move(event)),
      width(width),
      height(height),
      fps(fps) {}
bool Alarm::Finalize(Config& config) {
  Recorder recorder({width, height, fps});
  std::filesystem::path prefix_dir{config.upload_dir};
  prefix_dir /= "alarm";
  try {
    if (!std::filesystem::exists(prefix_dir)) {
      std::filesystem::create_directory(prefix_dir);
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cout << "genAlarmVideo() create_directory1 error:" << e.what()
              << std::endl;
    return false;
  }

  prefix_dir /= control_code;
  try {
    if (!std::filesystem::exists(prefix_dir)) {
      std::filesystem::create_directory(prefix_dir);
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cout << "genAlarmVideo() create_directory2 error:" << e.what()
              << std::endl;
    return false;
  }

  prefix_dir /= FrameWallClock::to_iso8601_utc(start_ts);

  try {
    if (!std::filesystem::exists(prefix_dir)) {
      std::filesystem::create_directory(prefix_dir);
    }
  } catch (std::filesystem::filesystem_error& e) {
    std::cout << "genAlarmVideo() create_directory3 error:" << e.what()
              << std::endl;
    return false;
  }

  if (event->more_segments || std::filesystem::exists(prefix_dir / "videos")) {
    prefix_dir /= "videos";
    try {
      if (!std::filesystem::exists(prefix_dir)) {
        std::filesystem::create_directory(prefix_dir);
      }
    } catch (std::filesystem::filesystem_error& e) {
      std::cout << "genAlarmVideo() create_directory3 error:" << e.what()
                << std::endl;
      return false;
    }
  }
  prefix_dir /= std::to_string(event->frames[1]->data->best_effort_timestamp) + ".ts";
  //    std::string image_path_abs = config.upload_dir + "/" + image_path;
  std::string path = prefix_dir.c_str();
  if (!recorder.Initialize(path, true)) {
    std::cout << "genAlarm can not initialize recorder_ error" << std::endl;
    return false;
  }

  // 上传报警信息start
  std::string url = config.admin_host + "/api/postAddAlarm";
  Json::Value param;
  param["control_code"] = control_code;
  param["desc"] = "";
  param["video_path"] = path;
  param["image_path"] = path;

  std::string data = param.toStyledString();
  Request request;
  std::string response;
  request.post(url.data(), data.data(), response);

  LOGI("\n \t request:%s \n \t response:%s", url.data(), response.data());
  for (auto& frame : event->frames) {
    recorder.AddVideoFrame(frame.get()->data.get());
    // TODO: create cover image
  }
  return true;
}
}  // namespace avs_analyzer