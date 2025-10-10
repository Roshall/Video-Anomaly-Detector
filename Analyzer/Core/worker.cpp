#include "worker.h"

#include "Utils/common.h"
#include "Utils/log.h"
#include "algorithm.h"
#include "analyzer.h"
#include "annotator.h"
#include "av_pull_stream.h"
#include "av_push_stream.h"
#include "control.h"
#include "event.h"
#include "frame_recorder.h"
#include "alarm.h"
#include "scheduler.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
}

namespace avs_analyzer {

Worker::Worker(Scheduler* scheduler, std::unique_ptr<Monitor> monitor)
    : monitor_(std::move(monitor)), scheduler(scheduler) {
  if (monitor_) {
    monitor_->start_timestamp = getCurTimestamp();
  }
  LOGI("");
}

Worker::~Worker() {
  LOGI("code=%s,streamUrl=%s finished.", monitor_->code.data(),
       monitor_->stream_url.data());
  state_.store(false); // Signal threads to stop
  // Wake up any sleeping threads (important for queues with timeouts/semaphores)
  if (in_stream) in_stream->CloseConnect(); // This should unblock GetVideoPkt
  if (out_stream) out_stream->CloseConnect(); // This should unblock PopVideoFrame

  for (auto& thread : threads_) {
    if (thread && thread->joinable()) {
      thread->join(); // Wait for the thread to finish
    }
  }
  threads_.clear();
  // unique_ptr members will be automatically cleaned up
}
bool Worker::start(std::string& msg) {
  in_stream = std::make_unique<AvPullStream>(this);
  if (in_stream->Connect()) {
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
  analyzer = std::make_unique<Analyzer>(scheduler, monitor_.get());

  state_.store(true);
  threads_.reserve(3);
  threads_.push_back(std::make_unique<std::thread>([this]() { this->in_stream->Read();
  }));

  threads_.push_back(std::make_unique<std::thread>([this]() { this->DecodeVideo();
  }));

  if (monitor_->push_stream) {
    if (monitor_->video_index > -1) {
      threads_.push_back(std::make_unique<std::thread>([this]() {
        this->out_stream->EncodeVideo();
      }));
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

void Worker::DecodeVideo() {
  int width = in_stream->video_codec_ctx->width;
  int height = in_stream->video_codec_ctx->height;

  AVFramePtr frame_yuv420p{av_frame_alloc()};  // pkt->解码->frame

  std::vector<Detection> happen_detects;
  auto in_time_base = in_stream->stream->time_base;
  VideoOverlay graph;
  if (!GrabFrame(frame_yuv420p.get())) {
    LOGE("No data can be retrieve from stream. exist");
    return;
  }
  int out_width =
      monitor_->push_stream ? out_stream->width() : width;
  int out_height =
      monitor_->push_stream ? out_stream->height() : height;

  EventDetector checker{
      EventDetectorConfig(),
      *analyzer,
      {width, height, in_stream->video_codec_ctx->pix_fmt, out_width, out_height},
                        monitor_->roi_points,
      frame_yuv420p.get()};

  std::shared_ptr<Recorder> recorder;
//  std::shared_ptr<Recorder> tester{std::make_shared<Recorder>(Recorder::VideoConfig{width, height})};
//  tester->Initialize("/tmp/test.ts");

  while (getState() && monitor_->video_index > -1) {
    av_frame_unref(frame_yuv420p.get());
    if (!GrabFrame(frame_yuv420p.get())) {
      continue;
    }
    //TODO: real time
    auto timestamp = frame_yuv420p->best_effort_timestamp;

    frame_yuv420p->time_base = in_time_base;
    frame_yuv420p->pict_type =  AV_PICTURE_TYPE_NONE;
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
        recorder = std::make_shared<Recorder>(Recorder::VideoConfig{width, height});
        // TODO: fire an alarm start to server
        break;
      }
      case EventDetector::kEventSplit: {
        auto event = checker.Flush();
        assert(event && "event should exist");
        scheduler->AddAlarm(std::make_unique<Alarm>(
            getCurTimestamp(), monitor_->code, std::move(event), recorder));
        break;
      }
      case EventDetector::kEventEnd: {
        auto event = checker.Flush();
        assert(event && "event should exist");
        scheduler->AddAlarm(std::make_unique<Alarm>(
            getCurTimestamp(), monitor_->code, std::move(event), std::move(recorder)));
        // TODO: fire an alarm end to server
        break;
      }
      case EventDetector::kEventActive: {
        // TODO: check alert level by object num
        auto num = checker.num_detection();
        // TODO: check alert level by duration
        auto duration = checker.event_duration();
        // TODO: reset check's state when level is changed
        break;
      }
      default:
        break;
    }
  }
}

bool Worker::GrabFrame(AVFrame* frame) const {
  while(true) {
    auto ret = avcodec_receive_frame(in_stream->video_codec_ctx.get(), frame);
    if (ret == AVERROR(EAGAIN)) {
      // Decoder needs more data, so we need to send the next packet.
      auto pkg = in_stream->GetVideoPkt();
      if (!pkg) return false;
      if (ret = avcodec_send_packet(in_stream->video_codec_ctx.get(), pkg.get()); ret < 0) {
        log_error("worker cannot send a packet:", ret);
        return false;
      }
      continue; // Go back to try receiving again
    } else if (ret < 0) {
      // A real error
      log_error("worker cannot decode a packet:", ret);
      return false;
    }
    return true;
  }
}

}  // namespace avs_analyzer
