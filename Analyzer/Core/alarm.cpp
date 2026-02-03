#include "alarm.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <format>
#include <chrono>

#include "Utils/log.h"
#include "Utils/common.h"
#include "Utils/request.h"
#include "video_processing/frame_recorder.h"
#include "video_processing/frame_walk_time.h"
#include "video_processing/hls_utils.h"
#include "combined_event_manager.h"
#include "scheduler.h"

namespace avs_analyzer {

Alarm::Alarm(std::string upload_dir, std::string admin_host, int width, int height,
             int64_t id, int fps)
    : upload_dir(std::move(upload_dir)),
      admin_host(std::move(admin_host)),
      width(width),
      height(height),
      fps(fps),
      id(id),
      hls_recorder(nullptr) {}

void Alarm::AddFrameToHls(const Event& event) const {
  if (!hls_recorder) {
    LOGW("Alarm::AddFrameToHls: hls_recorder is null, skipping.");
    return;
  }
  
  if (event.frames.empty()) {
    LOGW("Alarm::AddFrameToHls: event has no frames, skipping.");
    return;
  }
  
  // Feed frames from this slice to the HLS encoder
  for (const auto& frame : event.frames) {
    if (frame && frame->data) {
      hls_recorder->AddVideoFrame(frame->data.get());
    }
  }
  
  LOGD("Alarm::AddFrameToHls: added {} frames to HLS recorder", event.frames.size());
}

void Alarm::Finalize(
    std::unique_ptr<Event> event,
    std::chrono::time_point<std::chrono::system_clock> start_ts,
    const std::string& video_code) const {
  if (hls_recorder) {
    // Check if this is an intermediate slice or final event
    if (event && event->more_segments) {
      // Intermediate slice: buffer frames to HLS
      AddFrameToHls(*event);
    } else {
      // Final slice: close and reencode
      FinializeHlsEvent(std::move(event), start_ts, video_code);
    }
  } else {
    // Direct MP4 recording (legacy mode)
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
}

void Alarm::FinializeHlsEvent(
    std::unique_ptr<Event> event,
    std::chrono::time_point<std::chrono::system_clock> start_ts,
    const std::string& video_code) const {
  if (!hls_recorder) {
    LOGW("Alarm::FinializeHlsEvent: hls_recorder is null, skipping.");
    return;
  }

  // Add final slice frames before closing
  if (event && !event->frames.empty()) {
    AddFrameToHls(*event);
  }

  // Close the HLS recorder to finalize segments (synchronous)
  hls_recorder->CloseOutputSegment();
  
  // Get HLS paths from recorder
  if (!hls_recorder->last_hls_paths.has_value()) {
    LOGE("Alarm::FinializeHlsEvent: no HLS paths available");
    hls_recorder.reset();  // Reset recorder
    return;
  }
  
  // Copy data needed for async task
  const auto hls_paths = hls_recorder->last_hls_paths.value();
  const std::string start_time = FrameWallClock::to_iso8601_utc(start_ts);
  const std::string upload_dir_copy = upload_dir;
  const std::string admin_host_copy = admin_host;
  const int64_t alarm_id = id;
  const std::string video_code_copy = video_code;
  
  // Reset recorder now that it's closed
  hls_recorder.reset();
  
  LOGI("Alarm::FinializeHlsEvent: submitting async reencode task for video_code: {}", video_code);

  // Submit async reencode and reporting task to global thread pool
  g_th_pool.detach_task([hls_paths, start_time, upload_dir_copy, admin_host_copy, 
                         alarm_id, video_code_copy]() {
    // Reencode HLS to MP4
    ReencodeConfig reencode_cfg;
    bool reencode_success = ReencodeHlsToSuggestedMp4(hls_paths, reencode_cfg);

    if (!reencode_success) {
      LOGE("Alarm async reencode: ReencodeHlsToSuggestedMp4 failed for video_code: {}", video_code_copy);
      return;
    }

    const std::string& mp4_path = hls_paths.mp4_output;
    auto file_size = getFileSize(mp4_path);

    // Report MP4 to admin
    if (!admin_host_copy.empty()) {
      std::string url = std::format("{}/api/postAddAlarmVideo", admin_host_copy);
      
      // Make path relative to upload_dir
      std::string relative_path = mp4_path;
      if (relative_path.find(upload_dir_copy) == 0) {
        relative_path.erase(0, upload_dir_copy.length() + 1);
      }

      nlohmann::json param;
      param["videoCode"] = video_code_copy;
      param["videoPath"] = relative_path;
      param["alarmId"] = std::to_string(alarm_id);
      param["createTime"] = start_time;
      param["fileSize"] = file_size;

      const std::string data = param.dump();
      std::string response;
      Request::post(url.data(), data.data(), response);

      LOGI("Alarm async HLS-to-MP4 report success: request:{} response:{}", url, response);
    } else {
      LOGW("Alarm async reencode: admin_host is empty, skipping reporting");
    }
  });
}
}  // namespace avs_analyzer
