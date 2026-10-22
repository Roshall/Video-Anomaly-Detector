#ifndef ANALYZER_AVPUSHSTREAM_H
#define ANALYZER_AVPUSHSTREAM_H
#include <semaphore>
#include <memory>
#include <chrono>

#include "frame_recorder.h"
extern "C" {
#include "libavformat/avformat.h"
}
#include <readerwriterqueue/readerwriterqueue.h>
namespace avs_analyzer {
using namespace moodycamel;
class Worker;
struct Frame;

class AvPushStream {
 public:
  explicit AvPushStream(Worker* worker);
  ~AvPushStream();
  AvPushStream(const AvPushStream&) = delete;
  AvPushStream operator=(const AvPushStream&) = delete;

  bool Connect();       // 连接流媒体服务
  bool Reconnect();     // 重连流媒体服务
  void CloseConnect();  // 关闭流媒体服务的连接
  int connect_count = 0;
  AVStream* video_stream = nullptr;
  int video_index = -1;
  void AddVideoFrame(std::shared_ptr<const Frame> frame);
  int GetVideoFrameQSize();
  void SignalShutdown();
  void ClearVideoFrameQueue();
  int width() const;
  int height() const;

  // 编码视频帧并推流
  void EncodeVideo();

 private:
  Worker* worker_;
  Recorder recorder_;

  // Bounded queue: newest frames are dropped when the consumer falls behind.
  int max_frames_{50};
  std::chrono::steady_clock::time_point last_drop_log_{};

  // 视频帧
  ReaderWriterQueue<std::shared_ptr<const Frame>> video_frame_q_;
  std::counting_semaphore<> video_frame_cs_{0};
  std::shared_ptr<const Frame> PopVideoFrame();
};

}  // namespace avs_analyzer
#endif  // ANALYZER_AVPUSHSTREAM_H
