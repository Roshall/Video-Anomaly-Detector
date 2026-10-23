#include "alarm.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <format>
#include <chrono>
#include <semaphore>
#include <string>

#include "Utils/log.h"
#include "Utils/common.h"
#include "Utils/request.h"
#include "metrics.h"
#include "video_processing/frame_recorder.h"
#include "video_processing/frame_walk_time.h"
#include "video_processing/hls_utils.h"
#include "combined_event_manager.h"
#include "scheduler.h"

namespace avs_analyzer {

namespace {
// A single long HLS reencode must not consume every alarm I/O thread.
std::counting_semaphore<> g_reencode_slots{1};
}
HlsEventWriter::HlsEventWriter(int width, int height, int fps,
                               std::string base_path,
                               const Recorder::HlsConfig& hls_config)
    : recorder_(std::make_unique<Recorder>(Recorder::VideoConfig{width, height, fps})) {
  if (recorder_->Initialize(base_path, Recorder::OutputType::kHls, hls_config)) {
    paths_ = recorder_->last_hls_paths;
  } else {
    LOGW("HlsEventWriter: failed to initialize HLS recorder at {}", base_path);
    recorder_.reset();
  }
}

std::optional<Recorder::HlsPaths> HlsEventWriter::paths() const {
  if (!recorder_) {
    return paths_;
  }
  return recorder_->last_hls_paths;
}

void HlsEventWriter::AddFrames(const Event& event) {
  if (!recorder_ || event.frames.empty()) {
    return;
  }
  for (const auto& frame : event.frames) {
    if (frame && frame->data) {
      recorder_->AddVideoFrame(frame->data.get());
    }
  }
  LOGD("HlsEventWriter: added {} frames to HLS recorder", event.frames.size());
}

std::optional<Recorder::HlsPaths> HlsEventWriter::Close() {
  if (!recorder_) {
    return paths_;
  }
  recorder_->CloseOutputSegment();
  if (recorder_->last_hls_paths.has_value()) {
    paths_ = recorder_->last_hls_paths;
  }
  recorder_.reset();
  return paths_;
}

void Alarm::PostAlarmVideoToAdmin(const std::string& video_code,
                                   const std::string& video_type,
                                   const std::string& video_path,
                                   const std::string& create_time,
                                   int64_t alarm_id, int64_t file_size) const {
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
    {"alarmId", alarm_id},
    {"createTime", create_time},
    {"file", report_delta}
  };
  
  if (file_size > 0) {
    param["fileSize"] = file_size;
  }
  
  const std::string data = param.dump();
  std::string response;
  if (!Request::post(url.data(), data.data(), response)) {
    MetricsRegistry::Instance().alarm().upload_failures.fetch_add(
        1, std::memory_order_relaxed);
  }
  
  LOGI("Alarm::PostAlarmVideoToAdmin: {} video reported - request:{} response:{}", video_type, url, response);
}

Alarm::Alarm(std::string upload_dir, std::string admin_host, int width, int height,
             int fps, OutputMode mode, bool report_delta)
    : upload_dir(std::move(upload_dir)),
      admin_host(std::move(admin_host)),
      width(width),
      height(height),
      fps(fps),
      report_delta(report_delta),
      output_mode(mode) {}

void Alarm::ReportHlsToAdmin(const std::string& playlist_path,
                             std::chrono::time_point<std::chrono::system_clock> start_ts,
                             const std::string& video_code, int64_t alarm_id) const {
  if (admin_host.empty()) {
    return;
  }
  std::string start_time = FrameWallClock::to_iso8601_utc(start_ts);
  PostAlarmVideoToAdmin(video_code, "hls", playlist_path, start_time, alarm_id);
}

void Alarm::Finalize(AlarmJob&& job) const {
  if (job.hls_playlist) {
    ReportHlsToAdmin(job.hls_playlist->playlist, job.start_ts, job.video_code,
                     job.id);
  }

  if (job.hls_paths) {
    if (output_mode == OutputMode::kMp4Only) {
      return;
    }
    const auto hls_paths = *job.hls_paths;
    const std::string start_time = FrameWallClock::to_iso8601_utc(job.start_ts);
    const std::string video_code = job.video_code;
    const int64_t alarm_id = job.id;
    const bool is_file = report_delta;
    LOGI("Alarm::Finalize: submitting async reencode task for video_code: {}",
         video_code);
    g_alarm_pool.detach_task([hls_paths, start_time, upload_dir = upload_dir,
                           admin_host = admin_host, alarm_id, video_code,
                           is_file]() {
      g_reencode_slots.acquire();
      [[maybe_unused]] struct SlotRelease {
        ~SlotRelease() { g_reencode_slots.release(); }
      } slot_release;
      ReencodeConfig reencode_cfg;
      if (!ReencodeHlsToSuggestedMp4(hls_paths, reencode_cfg)) {
        MetricsRegistry::Instance().alarm().reencode_failures.fetch_add(
            1, std::memory_order_relaxed);
        LOGE("Alarm async reencode: ReencodeHlsToSuggestedMp4 failed for video_code: {}",
             video_code);
        return;
      }

      const std::string& mp4_path = hls_paths.mp4_output;
      auto file_size = getFileSize(mp4_path);
      Alarm temp_alarm(upload_dir, admin_host, 0, 0, 0, OutputMode::kAuto,
                       is_file);
      temp_alarm.PostAlarmVideoToAdmin(video_code, "mp4", mp4_path, start_time,
                                       alarm_id, file_size);
      if (!RemoveHlsFiles(hls_paths)) {
        LOGW("Alarm async cleanup: RemoveHlsFiles reported failures for {}",
             hls_paths.playlist);
      }
    });
    return;
  }

  if (job.captured_by_hls) {
    return;
  }

  FinalizeMp4Event(job.event.get(), job.start_ts, job.video_code, job.id);
}

void Alarm::FinalizeMp4Event(
    const Event* event,
    std::chrono::time_point<std::chrono::system_clock> start_ts,
    const std::string& video_code, int64_t alarm_id) const {
  if (!event || event->frames.empty()) {
    LOGW("Alarm::FinalizeMp4Event: empty event or no frames, skipping.");
    return;
  }

  Recorder recorder({width, height, fps});
  std::filesystem::path prefix_dir{upload_dir};
  std::string start_time = report_delta ? std::to_string(getCurTimestamp()) : FrameWallClock::to_iso8601_utc(start_ts);
  prefix_dir = prefix_dir / "alarms" / "videos" / video_code / start_time;

  if (!std::filesystem::exists(prefix_dir)) {
    try {
      std::filesystem::create_directories(prefix_dir);
    } catch (std::filesystem::filesystem_error& e) {
      LOGE("Alarm::FinalizeMp4Event create_directories error: {}", e.what());
    }
  }

  const int64_t file_ts =
      (event->frames.front() && event->frames.front()->data)
          ? event->frames.front()->data->best_effort_timestamp
          : event->start_timestamp;
  auto video_path = std::format("{}/{}.mp4", prefix_dir.string(), file_ts);

  if (!recorder.Initialize(video_path, Recorder::OutputType::kFile)) {
    LOGE("Alarm::FinalizeMp4Event: cannot initialize recorder at {}", video_path);
    return;
  }

  for (auto& frame : event->frames) {
    if (frame && frame->data) {
      recorder.AddVideoFrame(frame->data.get());
    }
  }
  recorder.CloseOutputSegment();
  auto file_size = getFileSize(video_path);

  // report saved video
  PostAlarmVideoToAdmin(video_code, "mp4", video_path,
                        report_delta
                            ? std::format("{:.3f}",
                                          std::chrono::duration<double>(
                                              start_ts.time_since_epoch())
                                              .count())
                            : start_time,
                        alarm_id, file_size);
}
}  // namespace avs_analyzer
