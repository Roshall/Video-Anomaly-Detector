#include "alarm.h"

#include <json/json.h>

#include <filesystem>
#include <iostream>

#include "Utils/log.h"
#include "Utils/request.h"
#include "frame_recorder.h"
#include "frame_walk_time.h"

namespace avs_analyzer {

Alarm::Alarm(std::chrono::time_point<std::chrono::system_clock> happenTimestamp,
             std::string controlCode, std::unique_ptr<Event> event,
             int width, int height, int fps)
    : start_ts(happenTimestamp),
      control_code(std::move(controlCode)),
      event(std::move(event)),
      width(width),
      height(height),
      fps(fps) {}

void Alarm::Finalize(const std::string& upload_dir,
                     const std::string& admin_host) const {
  Recorder recorder({width, height, fps});
  std::filesystem::path prefix_dir{upload_dir};
  std::string start_time = FrameWallClock::to_iso8601_utc(start_ts);
  prefix_dir = prefix_dir / "alarm" / control_code / start_time;


  prefix_dir /= "videos";
  if (!std::filesystem::exists(prefix_dir)) {
    try {
      std::filesystem::create_directories(prefix_dir);
    } catch (std::filesystem::filesystem_error& e) {
      std::cout << "genAlarmVideo() create_directorys error:" << e.what()
                << std::endl;
    }
  }
  prefix_dir /= std::to_string(event->frames[0]->data->best_effort_timestamp) + ".ts";
  std::string path = prefix_dir.c_str();
  if (!recorder.Initialize(path, true)) {
    std::cout << "genAlarm can not initialize recorder_ error" << std::endl;
  }

  // report saved video
  if (!admin_host.empty()) {
    std::string url = std::format("{}/api/postAddAlarmVideo", admin_host );
    Json::Value param;
    param["video_id"] = control_code;
    param["desc"] = "";
    param["video_path"] = path;
    param["create_time"] =   start_time;

    std::string data = param.toStyledString();
    std::string response;
    Request::post(url.data(), data.data(), response);

    LOGI("\n \t request:{} \n \t response:{}", url.data(), response.data());
    for (auto& frame : event->frames) {
      recorder.AddVideoFrame(frame->data.get());
    }
  }
}
}  // namespace avs_analyzer