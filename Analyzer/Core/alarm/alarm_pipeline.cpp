#include "alarm/alarm_pipeline.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <spdlog/fmt/fmt.h>
#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "Utils/common.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "config.h"
#include "event.h"
#include "scheduler.h"

namespace avs_analyzer {

AlarmPipeline::AlarmPipeline(Scheduler* scheduler, std::string video_code,
                             bool file, FrameWallClock& timer)
    : scheduler_(scheduler),
      video_code_(std::move(video_code)),
      file_(file),
      timer_(timer) {}

AlarmPipeline::~AlarmPipeline() = default;

void AlarmPipeline::Configure(int width, int height, int fps, int out_width,
                              int out_height) {
  width_ = width;
  height_ = height;
  fps_ = std::max(1, fps);
  out_width_ = out_width > 0 ? out_width : width;
  out_height_ = out_height > 0 ? out_height : height;
  if (!scheduler_) {
    return;
  }
  const auto& conf = scheduler_->GetConfig();
  alarm_settings_ = std::make_shared<const Alarm>(
      conf.upload_dir, conf.admin_host, out_width_, out_height_, fps_,
      file_ ? Alarm::OutputMode::kMp4Only : Alarm::OutputMode::kAuto, file_);
  pict_writer_ = std::make_shared<JPEGEncoder>(width_, height_);
}

void AlarmPipeline::StartHlsWriter() {
  hls_writer_.reset();
  hls_playlist_reported_ = false;
  hls_start_ts_ = {};
  if (file_ || !scheduler_ || scheduler_->GetConfig().upload_dir.empty()) {
    return;
  }
  std::string start_time =
      FrameWallClock::to_iso8601_utc(std::chrono::system_clock::now());
  std::filesystem::path hls_dir =
      std::filesystem::path(scheduler_->GetConfig().upload_dir) / "alarms" /
      "videos" / video_code_ / start_time;
  std::error_code ec;
  std::filesystem::create_directories(hls_dir, ec);
  if (ec) {
    LOGW("Failed to create HLS directory {}: {}", hls_dir.string(),
         ec.message());
    return;
  }
  auto writer = std::make_unique<HlsEventWriter>(
      width_, height_, fps_, (hls_dir / "stream").string(),
      scheduler_->GetConfig().hls_config);
  if (writer->valid()) {
    hls_writer_ = std::move(writer);
    LOGI("HLS recorder initialized for stream {}", video_code_);
  } else {
    LOGW("Failed to initialize HLS recorder for stream {}", video_code_);
  }
}

void AlarmPipeline::OnTransition(
    CombinedEventManager::Result::Transition transition, int64_t event_id,
    int64_t pts) {
  switch (transition) {
    case CombinedEventManager::Result::Transition::kBecameActive:
      if (!stream_active_.exchange(true, std::memory_order_acq_rel)) {
        NotifyAdminStreamEvent("start");
        last_combined_start_pts_ = pts;
        StartHlsWriter();
        LOGI("combined event {} started on {}", event_id, video_code_);
      }
      break;
    case CombinedEventManager::Result::Transition::kBecameIdle:
      if (stream_active_.exchange(false, std::memory_order_acq_rel)) {
        NotifyAdminStreamEvent("end");
      }
      break;
    default:
      break;
  }
}

void AlarmPipeline::EmitImage(const AlarmRequest& request) {
  if (!request.frame || !request.frame->data) {
    LOGW("AlarmImage received empty frame for monitor {}",
         request.monitor_code);
    return;
  }
  LOGI("检测到异常，发送报警图片到后台视频管理平台，摄像头： {}，监控点：{}，警报id： {}， 等级：{}",
       video_code_, request.monitor_code, request.alarm_id, request.level);
  const auto event_timestamp = timer_.compute(request.event_pts);
  const auto start_timestamp = timer_.compute(request.start_pts);

  // Encode before enqueueing the job so the pending alarm does not retain a
  // full annotated frame.
  std::vector<uint8_t> encoded;
  try {
    if (pict_writer_) {
      encoded = pict_writer_->EncodeToBuffer(request.frame->data.get());
    }
  } catch (const std::exception& e) {
    LOGE("AlarmImage: failed to encode picture: {}", e.what());
    return;
  }
  if (encoded.empty()) {
    LOGE("AlarmImage: failed to encode picture for monitor {}",
         request.monitor_code);
    return;
  }

  if (!scheduler_ || !scheduler_->SubmitAlarmIo(
          [conf = scheduler_->GetConfig(), code = request.monitor_code,
           start_timestamp, event_timestamp, level = request.level,
           alarm_id = request.alarm_id, video_code = video_code_,
           encoded = std::move(encoded), is_file = file_]() {
        auto img_path = fmt::format(
            "{}/alarms/images/{}/{}", conf.upload_dir, code,
            is_file ? std::to_string(getCurTimestamp())
                    : FrameWallClock::to_iso8601_utc(start_timestamp));
        std::filesystem::create_directories(
            std::filesystem::path(img_path));
        auto time_str = FrameWallClock::to_iso8601_utc(event_timestamp);
        auto img_path_str = fmt::format("{}/{}.jpg", img_path, time_str);
        std::ofstream out(img_path_str, std::ios::binary);
        if (!out.is_open()) {
          LOGE("failed to open alarm image {}", img_path_str);
          return;
        }
        out.write(reinterpret_cast<const char*>(encoded.data()),
                  static_cast<std::streamsize>(encoded.size()));
        if (!out) {
          LOGE("failed to write alarm image {}", img_path_str);
          return;
        }
        out.close();
        LOGI("Saved alarm image {} ({} bytes)", img_path_str, encoded.size());

        const std::string url =
            fmt::format("{}/api/postAddAlarm", conf.admin_host);
        nlohmann::json param{
            {"imagePath", img_path_str.substr(conf.upload_dir.length() + 1)},
            {"controlCode", code},
            {"videoCode", video_code},
            {"alarmId", alarm_id},
            {"createTime",
             is_file ? fmt::format("{:.3f}",
                                   std::chrono::duration<double>(
                                       event_timestamp.time_since_epoch())
                                       .count())
                     : FrameWallClock::to_iso8601_utc(event_timestamp)},
            {"level", level},
            {"file", is_file}};
        const std::string data = param.dump();
        std::string response;
        Request::post(url.data(), data.data(), response);
        LOGI("sent alarm image to {}", conf.admin_host);
      })) {
    LOGW("alarm image queue is full; dropping image for monitor {}",
         request.monitor_code);
  }
}

void AlarmPipeline::SubmitEvent(std::unique_ptr<Event> event, int64_t alarm_id,
                                int64_t start_pts) {
  if (!event || !scheduler_ || !alarm_settings_) {
    return;
  }
  AlarmJob job;
  job.alarm = alarm_settings_;
  job.id = alarm_id;
  job.video_code = video_code_;
  job.start_ts = timer_.compute(start_pts);

  if (hls_writer_) {
    const bool more_segments = event->more_segments;
    hls_writer_->AddFrames(*event);
    job.captured_by_hls = true;
    if (!hls_playlist_reported_) {
      if (auto playlist = hls_writer_->paths()) {
        job.hls_playlist = *playlist;
        hls_playlist_reported_ = true;
      }
    }
    if (hls_start_ts_ == std::chrono::system_clock::time_point{}) {
      hls_start_ts_ = job.start_ts;
    }
    if (!more_segments) {
      job.hls_paths = hls_writer_->Close();
      hls_writer_.reset();
      if (job.hls_paths) {
        job.start_ts = hls_start_ts_;
      }
    }
  }
  job.event = std::move(event);
  scheduler_->AddAlarm(std::move(job));
}

void AlarmPipeline::CloseHlsIntoJob(
    AlarmJob& job, int64_t id,
    std::chrono::system_clock::time_point fallback_start) {
  job.alarm = alarm_settings_;
  job.id = id;
  job.video_code = video_code_;
  job.start_ts = hls_start_ts_ == std::chrono::system_clock::time_point{}
                     ? fallback_start
                     : hls_start_ts_;
  if (hls_writer_) {
    job.captured_by_hls = true;
    job.hls_paths = hls_writer_->Close();
    hls_writer_.reset();
    if (!job.hls_paths) {
      job.start_ts = fallback_start;
    }
  }
}

void AlarmPipeline::OnGeometryChanged(int width, int height, int fps,
                                      int out_width, int out_height,
                                      int64_t active_event_id) {
  if (hls_writer_) {
    AlarmJob job;
    CloseHlsIntoJob(job, active_event_id, std::chrono::system_clock::now());
    if (job.hls_paths) {
      scheduler_->AddAlarm(std::move(job));
    }
  }
  Configure(width, height, fps, out_width, out_height);
  LOGI("worker {} stream geometry changed to {}x{}", video_code_, width,
       height);
}

void AlarmPipeline::FinalizeOnStreamEnd(
    bool had_active, std::vector<std::unique_ptr<Event>> tail_events,
    int64_t tail_id, bool canceled_all) {
  if (had_active &&
      stream_active_.exchange(false, std::memory_order_acq_rel)) {
    NotifyAdminStreamEvent("end");
  }

  if (had_active && tail_events.empty() && canceled_all) {
    const auto start_ts =
        last_combined_start_pts_ > 0
            ? timer_.compute(last_combined_start_pts_)
            : std::chrono::system_clock::now();
    AlarmJob job;
    CloseHlsIntoJob(job, tail_id, start_ts);
    scheduler_->AddAlarm(std::move(job));
  }

  for (auto& event : tail_events) {
    if (!event) {
      continue;
    }
    const int64_t start_pts = event->start_timestamp;
    SubmitEvent(std::move(event), tail_id, start_pts);
  }
}

void AlarmPipeline::DeactivateStream() {
  if (stream_active_.exchange(false, std::memory_order_acq_rel)) {
    NotifyAdminStreamEvent("end");
  }
}

void AlarmPipeline::NotifyAdminStreamEvent(std::string_view state) {
  if (!scheduler_) {
    return;
  }
  const auto& conf = scheduler_->GetConfig();
  if (conf.admin_host.empty() || video_code_.empty()) {
    return;
  }
  const std::string url = fmt::format(
      "{}/api/stream_event?state={}&streamCode={}", conf.admin_host, state,
      video_code_);
  // Fire-and-forget async request to avoid blocking the DetectEvent loop.
  g_control_pool.detach_task([url]() {
    std::string response;
    Request::get(url.c_str(), response);
    LOGI("sent stream event to {}, response: {}", url, response);
  });
}

}  // namespace avs_analyzer
