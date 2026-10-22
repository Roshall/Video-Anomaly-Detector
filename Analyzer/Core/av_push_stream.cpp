#include "av_push_stream.h"

#include "Utils/log.h"
#include "frame_recorder.h"
#include "scheduler.h"
#include "worker.h"
#include "frame.h"

extern "C" {
#include <libswresample/swresample.h>
}

namespace avs_analyzer {
AvPushStream::AvPushStream(Worker* worker)
    : worker_(worker),
      recorder_(
          {worker->VideoWidth(), worker->VideoHeight()}),
      max_frames_(worker->PushQueueMaxFrames()) {}

AvPushStream::~AvPushStream() { CloseConnect(); }

bool AvPushStream::Connect() {
  return recorder_.Initialize(worker_->PushStreamUrl(), Recorder::OutputType::kRtsp);
}
bool AvPushStream::Reconnect() {
  if (connect_count <= 100) {
    CloseConnect();
    return Connect();
  }
  return false;
}
void AvPushStream::CloseConnect() {
  ClearVideoFrameQueue();
  SignalShutdown();
  recorder_.CloseOutputSegment();
  //  fmt_ctx.reset();
}

void AvPushStream::AddVideoFrame(std::shared_ptr<const Frame> frame) {
  if (static_cast<int>(video_frame_q_.size_approx()) >= max_frames_) {
    worker_->RecordPushFrameDrop();
    const auto now = std::chrono::steady_clock::now();
    if (last_drop_log_ == std::chrono::steady_clock::time_point{} ||
        now - last_drop_log_ >= std::chrono::seconds(1)) {
      last_drop_log_ = now;
      LOGW("Push queue full ({} frames), dropping newest frame", max_frames_);
    }
    return;
  }
  video_frame_q_.enqueue(std::move(frame));
  video_frame_cs_.release();
}
int AvPushStream::GetVideoFrameQSize() { return video_frame_q_.size_approx(); }

std::shared_ptr<const Frame> AvPushStream::PopVideoFrame() {
  video_frame_cs_.acquire();
  std::shared_ptr<const Frame> frame;
  video_frame_q_.try_dequeue(frame);
  return frame;
}
void AvPushStream::ClearVideoFrameQueue() {
  std::shared_ptr<const Frame> video_frame;
  while (video_frame_cs_.try_acquire() &&
         video_frame_q_.try_dequeue(video_frame)) {
  }
}
void AvPushStream::EncodeVideo() {
  int continuity_error_count = 0;

  while (worker_->State()) {
    if (auto frame = PopVideoFrame()) {
      if (not recorder_.AddVideoFrame(frame->data.get())) {
        continuity_error_count++;
        if (continuity_error_count > 5) {  // 大于5秒重启拉流连接

          LOGE("av_read_frame error, continuity_error_count = {} (s)",
               continuity_error_count);

          if (Reconnect()) {
            continuity_error_count = 0;
            LOGI("reConnect success : mConnectCount={}", connect_count);
          } else {
            LOGI("reConnect error : mConnectCount={}", connect_count);
            CloseConnect();
            // Cleanup must not join from this encoder thread.
            worker_->CleanUpPushThread(true);
            return;
          }
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
      }
    }
  }
}
void AvPushStream::SignalShutdown() { video_frame_cs_.release(); }
int AvPushStream::width() const { return recorder_.config.width; }
int AvPushStream::height() const { return recorder_.config.height; }

}  // namespace avs_analyzer
