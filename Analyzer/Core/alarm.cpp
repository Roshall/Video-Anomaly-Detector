#include "alarm.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <format>
#include <chrono>

#include "Utils/log.h"
#include "Utils/request.h"
#include "frame_recorder.h"
#include "frame_walk_time.h"
#include "combined_event_manager.h"

namespace avs_analyzer {

Alarm::Alarm(std::string upload_dir, std::string admin_host, int width, int height,
             int fps)
    : upload_dir(std::move(upload_dir)),
      admin_host(std::move(admin_host)),
      width(width),
      height(height),
      fps(fps) {}

void Alarm::Finalize(
    std::unique_ptr<Event> event,
    std::chrono::time_point<std::chrono::system_clock> start_ts,
    const std::string& monitor_code) const {
  if (!event || event->frames.empty()) {
    LOGW("Alarm::Finalize: empty event or no frames, skipping.");
    return;
  }

  Recorder recorder({width, height, fps});
  std::filesystem::path prefix_dir{upload_dir};
  std::string start_time = FrameWallClock::to_iso8601_utc(start_ts);
  prefix_dir = prefix_dir / "alarm" / monitor_code / start_time;

  prefix_dir /= "videos";
  if (!std::filesystem::exists(prefix_dir)) {
    try {
      std::filesystem::create_directories(prefix_dir);
    } catch (std::filesystem::filesystem_error& e) {
      LOGE("Alarm::Finalize create_directories error: {}", e.what());
    }
  }

  prefix_dir /=
      std::to_string(event->frames[0]->data->best_effort_timestamp) + ".ts";
  std::string path = prefix_dir.string();

  if (!recorder.Initialize(path, true)) {
    LOGE("Alarm::Finalize: cannot initialize recorder at {}", path);
    return;
  }

  for (auto& frame : event->frames) {
    recorder.AddVideoFrame(frame->data.get());
  }

  // report saved video
  if (!admin_host.empty()) {
    std::string url = std::format("{}/api/postAddAlarmVideo", admin_host);
    nlohmann::json param;
    param["video_id"] = monitor_code;
    param["desc"] = "";
    param["video_path"] = path;
    param["create_time"] = start_time;

    const std::string data = param.dump();
    std::string response;
    Request::post(url.data(), data.data(), response);

    LOGI("Alarm::Finalize report success: request:{} response:{}", url, response);
  } else {
    LOGW("Alarm::Finalize: admin_host is empty, skipping reporting for {}", path);
  }
}
}  // namespace avs_analyzer
