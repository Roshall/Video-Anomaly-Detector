#include "worker.h"

#include <json/value.h>

#include <filesystem>

#include "Utils/common.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "alarm.h"
#include "algorithm.h"
#include "analyzer.h"
#include "av_pull_stream.h"
#include "av_push_stream.h"
#include "config.h"
#include "monitor.h"
#include "event.h"
#include "jpeg_encoder.h"
#include "scheduler.h"
#include "streamer.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
}

namespace avs_analyzer {

Worker::Worker(Scheduler* scheduler, std::unique_ptr<Monitor> monitor,
               const EventDetectorConfig& event_cfg)
    : monitor_(std::move(monitor)), scheduler(scheduler), event_cfg_(event_cfg) {
  if (monitor_) {
    monitor_->start_timestamp = getCurTimestamp();
  }
}

Worker::~Worker() {
  LOGI("code={},streamUrl={} finished.", monitor_->code.data(),
       monitor_->stream_url.data());
  state_.store(false);  // Signal threads to stop
  // Wake up any sleeping threads (important for queues with
  // timeouts/semaphores)
  if (in_stream) in_stream->CloseConnect();  // This should unblock GetVideoPkt
  if (out_stream)
    out_stream->CloseConnect();  // This should unblock PopVideoFrame

  for (auto& thread : threads_) {
    if (thread && thread->joinable()) {
      thread->join();  // Wait for the thread to finish
    }
  }
  threads_.clear();
  // unique_ptr members will be automatically cleaned up
}
bool Worker::start(std::string& msg, float conf_th) {
  if (monitor_->source_type == "file") {
    OfflineAnalyze();
  }
  in_stream = std::make_unique<AvPullStream>(this);
  if (in_stream->Connect(false)) {
    if (monitor_->push_stream) {
      out_stream = std::make_unique<AvPushStream>(this);
      if (out_stream->Connect()) {
        // success
      } else {
        msg = "pull stream connect success, push stream connect error";
        return false;
      }
    } else {
      // success
    }
  } else {
    msg = "pull stream connect error";
    return false;
  }
  analyzer = std::make_unique<Analyzer>(monitor_.get(), scheduler->GetConfig(), conf_th, in_stream->GetFps());

  timer_.init(in_stream->fmt_ctx.get(), in_stream->stream);

  state_.store(true);
  threads_.reserve(3);
  threads_.push_back(
      std::make_unique<std::thread>([this]() { this->in_stream->Read(); }));

  threads_.push_back(
      std::make_unique<std::thread>([this]() { this->DecodeVideo(); }));

  if (monitor_->push_stream) {
    if (monitor_->video_index > -1) {
      threads_.push_back(std::make_unique<std::thread>(
          [this]() { this->out_stream->EncodeVideo(); }));
    }
  }
  return true;
}

bool Worker::getState() const { return state_.load(); }
void Worker::remove() {
  state_.store(false);
  in_stream->CloseConnect();
  out_stream->CloseConnect();
  scheduler->RemoveWorker(monitor_->code);
}

void Worker::OfflineAnalyze() {
  auto in{Streamer(monitor_->stream_url)};
  int width = in.width();
  int height = in.height();
  auto in_time_base = in.time_base;
  auto example = in.NextFrame();
  EventDetector checker{
      EventDetectorConfig{.minor_stride_ms = 500, .min_vote = 2},
      *analyzer,
      {width, height, in_stream->video_codec_ctx->pix_fmt, width, height},
      monitor_->roi_points,
      example};

  JPEGEncoder pict_writer(width, height);
  while (!in.EoF()) {
    auto frame = in.NextFrame();
    auto timestamp = frame->best_effort_timestamp;

    frame->time_base = in_time_base;
    frame->pict_type = AV_PICTURE_TYPE_NONE;
    // the time base of timestamp / in_time_base is 's', we divide it by 1/1000
    // to has 'ms' time base
    auto state = checker.ProcessFrame(
        {av_rescale_q(timestamp, in_time_base, {1, 1000}), frame});

    auto upload_dir = scheduler->GetConfig().upload_dir;

    switch (state) {
      case EventDetector::kEventStart: {
        AlarmImage(checker, pict_writer, false);
        break;
      }
      case EventDetector::kEventSplit: {
        AlarmImage(checker, pict_writer, false);
        auto start_ts = timer_.compute(checker.start_ts());
        auto event = checker.Flush();
        assert(event && "event should exist");
        g_th_pool.detach_task([start_ts, code = monitor_->code,
                               event = std::move(event), width, height,
                               upload_dir]() mutable {
          Alarm alarm(start_ts, std::move(code), std::move(event), width,
                      height);
          alarm.Finalize(upload_dir, "");
        });
        break;
      }
      case EventDetector::kEventEnd: {
        auto event = checker.Flush();
        assert(event && "event should exist");
        auto ts = timer_.compute(event->start_timestamp);
        g_th_pool.detach_task([ts, code = monitor_->code,
                               event = std::move(event), width, height,
                               upload_dir]() mutable {
          Alarm alarm(ts, std::move(code), std::move(event), width, height);
          alarm.Finalize(upload_dir, "");
        });
        break;
      }
      default:
        break;
    }
  }
}

void Worker::AlarmImage(EventDetector& checker, JPEGEncoder& pict_writer,
                        bool push) noexcept {
  auto ts = timer_.compute(checker.timestamp());
  int64_t num;
  std::string_view level;
  if (push) {
    if (monitor_->level_mode == LevelMode::kNumPeople) {
      num = checker.num_detection();
    } else {
      num = checker.event_duration();
    }
    size_t i = 0;
    auto total_level = monitor_->levels.first.size();
    while (i < total_level && monitor_->levels.first[i] < num) {
      ++i;
    }
    if (i == 0) return;
    level = monitor_->levels.second[i - 1];
  }
  g_th_pool.detach_task([&pict_writer, this, start_ts = timer_.compute(checker.start_ts()),
                         ts, level, push, frame = checker.latest_frame()] {
    auto& conf = scheduler->GetConfig();
    auto img_path =
        std::format("{}/alarm/{}/{}", conf.upload_dir, monitor_->code,
                    FrameWallClock::to_iso8601_utc(start_ts));
    std::filesystem::create_directories(std::filesystem::path(img_path));
    auto time_str = FrameWallClock::to_iso8601_utc(ts);
    auto img_path_str = std::format("{}/{}.jpg", img_path, time_str);
    int ret = pict_writer.saveFrame(frame->data.get(), img_path_str);
    if (ret < 0) {
      LOGE_FFMPEG("failed to save picture", ret);
      return;
    } else {
      LOGI("save alarm image to {}", img_path_str);
    }

    if (push) {
      std::string url = std::format("{}/api/postAddAlarm", conf.admin_host);
      Json::Value param;
      param["control_code"] = monitor_->code;
      param["desc"] = "";
      param["img_path"] = img_path_str;
      param["create_time"] = time_str;
      param["level"] = level.data();

      std::string data = param.toStyledString();
      Request request;
      std::string response;
      request.post(url.data(), data.data(), response);
      std::cout << "sent alam image to " << conf.admin_host << std::endl;
    }
  });
}

void Worker::DecodeVideo() {
  int width = in_stream->video_codec_ctx->width;
  int height = in_stream->video_codec_ctx->height;

  AVFramePtr frame_yuv420p{av_frame_alloc()};  // pkt->解码->frame

  auto in_time_base = in_stream->stream->time_base;
  if (!GrabFrame(frame_yuv420p.get())) {
    LOGE("No data can be retrieve from stream. exist");
    return;
  }
  int out_width = monitor_->push_stream ? out_stream->width() : width;
  int out_height = monitor_->push_stream ? out_stream->height() : height;

  EventDetector checker{
      event_cfg_,
      *analyzer,
      {width, height, in_stream->video_codec_ctx->pix_fmt, out_width,
       out_height},
      monitor_->roi_points,
      frame_yuv420p.get()};

  JPEGEncoder pict_writer(width, height);

  while (getState() && monitor_->video_index > -1) {
    av_frame_unref(frame_yuv420p.get());
    if (!GrabFrame(frame_yuv420p.get())) {
      continue;
    }
    auto timestamp = frame_yuv420p->best_effort_timestamp;

    frame_yuv420p->time_base = in_time_base;
    frame_yuv420p->pict_type = AV_PICTURE_TYPE_NONE;
    // the time base of timestamp / in_time_base is 's', we divide it by 1/1000
    // to has 'ms' time base
    auto state =
        checker.ProcessFrame({av_rescale_q(timestamp, in_time_base, {1, 1000}),
                              frame_yuv420p.get()});
    if (monitor_->push_stream) {
      out_stream->AddVideoFrame(checker.latest_frame());
    }
    switch (state) {
      case EventDetector::kEventStart: {
        AlarmImage(checker, pict_writer, true);
        break;
      }
      case EventDetector::kEventSplit: {
        AlarmImage(checker, pict_writer, true);
        auto event = checker.Flush();
        assert(event && "event should exist");
        scheduler->AddAlarm(std::make_unique<Alarm>(
            timer_.compute(event->start_timestamp), monitor_->code,
            std::move(event), out_width, out_height));
        break;
      }
      case EventDetector::kEventEnd: {
        auto event = checker.Flush();
        assert(event && "event should exist");
        scheduler->AddAlarm(std::make_unique<Alarm>(
            timer_.compute(event->start_timestamp), monitor_->code,
            std::move(event), out_width, out_height));
        break;
      }
      default:
        break;
    }
  }
}

bool Worker::GrabFrame(AVFrame* frame) const {
  while (true) {
    auto ret = avcodec_receive_frame(in_stream->video_codec_ctx.get(), frame);
    if (ret == AVERROR(EAGAIN)) {
      // Decoder needs more data, so we need to send the next packet.
      auto pkg = in_stream->GetVideoPkt();
      if (!pkg) return false;
      if (ret =
              avcodec_send_packet(in_stream->video_codec_ctx.get(), pkg.get());
          ret < 0) {
        LOGE_FFMPEG("worker cannot send a packet:", ret);
        return false;
      }
      continue;  // Go back to try receiving again
    } else if (ret < 0) {
      // A real error
      LOGE_FFMPEG("worker cannot decode a packet:", ret);
      return false;
    }
    return true;
  }
}

}  // namespace avs_analyzer
