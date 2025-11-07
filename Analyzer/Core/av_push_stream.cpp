#include "av_push_stream.h"

#include "Utils/log.h"
#include "monitor.h"
#include "frame_recorder.h"
#include "worker.h"
#include "frame.h"

extern "C" {
#include <libswresample/swresample.h>
}

namespace avs_analyzer {
AvPushStream::AvPushStream(Worker* worker)
    : worker_(worker),
      recorder_(
          {worker->monitor_->video_width, worker->monitor_->video_height}) {}

AvPushStream::~AvPushStream() { CloseConnect(); }

bool AvPushStream::Connect() {
  return recorder_.Initialize(worker_->monitor_->push_stream_url, false);
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

void AvPushStream::AddVideoFrame(std::unique_ptr<Frame> frame) {
  if (video_frame_q_.size_approx() > 30 * 10) {
    LOGW("To many frame remains, throw away one");
    return;
  }
  video_frame_q_.enqueue(std::move(frame));
  video_frame_cs_.release();
}
int AvPushStream::GetVideoFrameQSize() { return video_frame_q_.size_approx(); }

std::unique_ptr<Frame> AvPushStream::PopVideoFrame() {
  video_frame_cs_.acquire();
  std::unique_ptr<Frame> frame;
  video_frame_q_.try_dequeue(frame);
  return frame;
}
void AvPushStream::ClearVideoFrameQueue() {
  std::shared_ptr<Frame> video_frame;
  while (video_frame_cs_.try_acquire() &&
         video_frame_q_.try_dequeue(video_frame)) {
    // delete video_frame;
  }
}
void AvPushStream::EncodeVideo() {
  int continuity_error_count = 0;

  while (worker_->getState()) {
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
            worker_->remove();
          }
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
      }
    }
  }

  // av_write_trailer(mFmtCtx);//写文件尾


}
void AvPushStream::SignalShutdown() { video_frame_cs_.release(); }
int AvPushStream::width() const { return recorder_.config.width; }
int AvPushStream::height() const { return recorder_.config.height; }

}  // namespace avs_analyzer
