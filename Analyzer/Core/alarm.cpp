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

void Alarm::PostAlarmVideoToAdmin(const std::string& video_code,
                                   const std::string& video_type,
                                   const std::string& video_path,
                                   const std::string& create_time,
                                   int64_t file_size) const {
  if (admin_host.empty()) {
    LOGW("Alarm::PostAlarmVideoToAdmin: admin_host is empty, skipping.");
    return;
  }

  std::string url = std::format("{}/api/postAddAlarmVideo", admin_host);
  
  // Make path relative to upload_dir
  std::string relative_path = video_path;
  if (relative_path.find(upload_dir) == 0) {
    relative_path.erase(0, upload_dir.length() + 1);
  }
  
  nlohmann::json param{
    {"videoCode", video_code},
    {"videoType", video_type},
    {"videoPath", relative_path},
    {"alarmId", std::to_string(id)},
    {"createTime", create_time}
  };
  
  if (file_size > 0) {
    param["fileSize"] = file_size;
  }
  
  const std::string data = param.dump();
  std::string response;
  Request::post(url.data(), data.data(), response);
  
  LOGI("Alarm::PostAlarmVideoToAdmin: {} video reported - request:{} response:{}", video_type, url, response);
}

Alarm::Alarm(std::string upload_dir, std::string admin_host, int width, int height,
             int64_t id, int fps, OutputMode mode)
    : upload_dir(std::move(upload_dir)),
      admin_host(std::move(admin_host)),
      width(width),
      height(height),
      fps(fps),
      id(id),
      output_mode(mode),
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

void Alarm::ReportHlsToAdmin(const std::string& playlist_path,
                             std::chrono::time_point<std::chrono::system_clock> start_ts,
                             const std::string& video_code) const {
  std::string start_time = FrameWallClock::to_iso8601_utc(start_ts);
  PostAlarmVideoToAdmin(video_code, "hls", playlist_path, start_time);
}

void Alarm::Finalize(
    std::unique_ptr<Event> event,
    std::chrono::time_point<std::chrono::system_clock> start_ts,
    const std::string& video_code) const {
  if (output_mode == OutputMode::kMp4Only) {
    if (hls_recorder) {
      LOGW("Alarm::Finalize: mp4-only mode ignores HLS recorder.");
      hls_recorder.reset();
    }
    FinalizeMp4Event(std::move(event), start_ts, video_code);
    return;
  }

  if (hls_recorder) {
    // Check if this is an intermediate slice or final event
    if (event && event->more_segments) {
      // Intermediate slice: buffer frames to HLS
      AddFrameToHls(*event);
      
      // Report HLS stream on first intermediate event (for live playback)
      if (!hls_reported && hls_recorder->last_hls_paths.has_value()) {
        const auto& playlist_path = hls_recorder->last_hls_paths.value().playlist;
        ReportHlsToAdmin(playlist_path, start_ts, video_code);
        alarm_start_time = start_ts;  // Track when the alarm was triggered for reporting purposes
        hls_reported = true;
      }
    } else {
      // Final slice: close and reencode
      FinializeHlsEvent(std::move(event), start_ts, video_code);
    }
  } else {
    if (alarm_start_time != std::chrono::system_clock::time_point{}) {
      start_ts = alarm_start_time;  // Use the original alarm trigger time for reporting
    }
    FinalizeMp4Event(std::move(event), start_ts, video_code);
  }
}

void Alarm::FinalizeMp4Event(
    std::unique_ptr<Event> event,
    std::chrono::time_point<std::chrono::system_clock> start_ts,
    const std::string& video_code) const {
  if (!event || event->frames.empty()) {
    LOGW("Alarm::FinalizeMp4Event: empty event or no frames, skipping.");
    return;
  }

  Recorder recorder({width, height, fps});
  std::filesystem::path prefix_dir{upload_dir};
  std::string start_time = FrameWallClock::to_iso8601_utc(start_ts);
  prefix_dir = prefix_dir / "alarms" / "videos" / video_code / start_time;

  if (!std::filesystem::exists(prefix_dir)) {
    try {
      std::filesystem::create_directories(prefix_dir);
    } catch (std::filesystem::filesystem_error& e) {
      LOGE("Alarm::FinalizeMp4Event create_directories error: {}", e.what());
    }
  }

  auto video_path = std::format("{}/{}.mp4", prefix_dir.string(),
                                event->frames[0]->data->best_effort_timestamp);

  if (!recorder.Initialize(video_path, Recorder::OutputType::kFile)) {
    LOGE("Alarm::FinalizeMp4Event: cannot initialize recorder at {}", video_path);
    return;
  }

  for (auto& frame : event->frames) {
    recorder.AddVideoFrame(frame->data.get());
  }
  recorder.CloseOutputSegment();
  auto file_size = getFileSize(video_path);

  // report saved video
  PostAlarmVideoToAdmin(video_code, "mp4", video_path, start_time, file_size);
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
  
  // Reset recorder now that it's closed
  hls_recorder.reset();
  
  LOGI("Alarm::FinializeHlsEvent: submitting async reencode task for video_code: {}", video_code);

  // Submit async reencode and reporting task to global thread pool
  g_th_pool.detach_task([hls_paths, start_time, upload_dir = upload_dir,
     admin_host = admin_host,
                         alarm_id = id, video_code]() {
    // Reencode HLS to MP4
    ReencodeConfig reencode_cfg;
    bool reencode_success = ReencodeHlsToSuggestedMp4(hls_paths, reencode_cfg);

    if (!reencode_success) {
      LOGE("Alarm async reencode: ReencodeHlsToSuggestedMp4 failed for video_code: {}", video_code);
      return;
    }

    const std::string& mp4_path = hls_paths.mp4_output;
    auto file_size = getFileSize(mp4_path);

    // Report MP4 to admin
    // Need to create a temporary Alarm object to call the helper function
    Alarm temp_alarm(upload_dir, admin_host, 0, 0, alarm_id, 0);
    temp_alarm.PostAlarmVideoToAdmin(video_code, "mp4", mp4_path, start_time, file_size);
  });
}
}  // namespace avs_analyzer
