#include "alarm.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <format>
#include <chrono>

#include "Utils/log.h"
#include "Utils/common.h"
#include "Utils/request.h"
#include "frame_recorder.h"
#include "frame_walk_time.h"
#include "combined_event_manager.h"

namespace avs_analyzer {

Alarm::Alarm(std::string upload_dir, std::string admin_host, int width, int height,
             int64_t id, int fps)
    : upload_dir(std::move(upload_dir)),
      admin_host(std::move(admin_host)),
      width(width),
      height(height),
      fps(fps),
      id(id) {}

void Alarm::Finalize(
    std::unique_ptr<Event> event,
    std::chrono::time_point<std::chrono::system_clock> start_ts,
    const std::string& video_code) const {
  if (!event || event->frames.empty()) {
    LOGW("Alarm::Finalize: empty event or no frames, skipping.");
    return;
  }

  Recorder recorder({width, height, fps});
  std::filesystem::path prefix_dir{upload_dir};
  std::string start_time = FrameWallClock::to_iso8601_utc(start_ts);
  prefix_dir = prefix_dir / "alarm" / video_code / start_time;

  prefix_dir /= "videos";
  if (!std::filesystem::exists(prefix_dir)) {
    try {
      std::filesystem::create_directories(prefix_dir);
    } catch (std::filesystem::filesystem_error& e) {
      LOGE("Alarm::Finalize create_directories error: {}", e.what());
    }
  }

  prefix_dir /=
      std::to_string(event->frames[0]->data->best_effort_timestamp);
  std::string path = prefix_dir.string();

  if (!recorder.Initialize(std::format("{}.mp4", path), Recorder::OutputType::kFile)) {
    LOGE("Alarm::Finalize: cannot initialize recorder at {}", path);
    return;
  }

  for (auto& frame : event->frames) {
    recorder.AddVideoFrame(frame->data.get());
  }
  recorder.CloseOutputSegment();
  auto file_size = getFileSize(std::format("{}.mp4", path));

  // report saved video
  if (!admin_host.empty()) {
    std::string url = std::format("{}/api/postAddAlarmVideo", admin_host);
    path.erase(0, upload_dir.length() + 1);  // remove upload_dir and leading '/'
    nlohmann::json param;
    param["videoCode"] = video_code;
    param["videoPath"] = std::format("{}.mp4", path);
    param["alarmId"] = std::to_string(id);
    param["createTime"] = start_time;
    param["fileSize"] = file_size;

    const std::string data = param.dump();
    std::string response;
    Request::post(url.data(), data.data(), response);

    LOGI("Alarm::Finalize report success: request:{} response:{}", url, response);
  } else {
    LOGW("Alarm::Finalize: admin_host is empty, skipping reporting for {}", path);
  }
}
}  // namespace avs_analyzer
