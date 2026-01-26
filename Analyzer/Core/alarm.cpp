#include "alarm.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>

#include "Utils/log.h"
#include "Utils/request.h"
#include "frame_recorder.h"
#include "jpeg_encoder.h"
#include "frame_walk_time.h"

namespace avs_analyzer {

Alarm::Alarm(std::chrono::time_point<std::chrono::system_clock> happenTimestamp,
             std::string controlCode, std::unique_ptr<Event> event, int width,
             int height, int64_t id, int fps)
    : start_ts(happenTimestamp),
      control_code(std::move(controlCode)),
      event(std::move(event)),
      width(width),
      height(height),
      fps(fps),
      id(id) {}

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
  std::string video_path = prefix_dir.string();
  if (!recorder.Initialize(video_path, true)) {
    std::cout << "genAlarm can not initialize recorder_ error" << std::endl;
  }

  // save first frame as JPEG next to the video (same name, .jpg extension)
  if (event && !event->frames.empty() && event->frames[0] && event->frames[0]->data) {
    try {
      std::filesystem::path image_path = prefix_dir;
      image_path.replace_extension(".jpg");
      std::string image_path_str = image_path.string();
      avs_analyzer::JPEGEncoder encoder(width, height);
      if (int r = encoder.saveFrame(event->frames[0]->data.get(), image_path_str); r < 0) {
        std::cout << "genAlarm failed to save first frame to jpg, error: " << r << std::endl;
      } else {
        LOGI("Saved first frame to {}", image_path_str);
      }
    } catch (const std::exception &e) {
      std::cout << "genAlarm save image error:" << e.what() << std::endl;
    }
  } else {
    std::cout << "genAlarm has no frames to save image" << std::endl;
  }

  // report saved video
  if (!admin_host.empty()) {
    std::string url = std::format("{}/api/postAddAlarmVideo", admin_host );
    nlohmann::json param;
    param["video_code"] = control_code;
    param["alarm_id"] = std::to_string(id);
    param["video_path"] = video_path;
    // add image_path if exists
    std::filesystem::path image_check = prefix_dir;
    image_check.replace_extension(".jpg");
    param["image_path"] = image_check.string();
    param["create_time"] = start_time;

    const std::string data = param.dump();
    std::string response;
    Request::post(url.data(), data.data(), response);

    LOGI("\n \t request:{} \n \t response:{}", url.data(), response.data());
    for (auto& frame : event->frames) {
      recorder.AddVideoFrame(frame->data.get());
    }
  }
}
}  // namespace avs_analyzer