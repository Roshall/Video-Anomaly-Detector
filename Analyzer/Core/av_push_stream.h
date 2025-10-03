#ifndef ANALYZER_AVPUSHSTREAM_H
#define ANALYZER_AVPUSHSTREAM_H
#include <mutex>
#include <queue>

#include "helper.h"

#include <semaphore>
extern "C" {
#include "libavcodec/avcodec.h"
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

  bool Connect();       // 连接流媒体服务
  bool Reconnect();     // 重连流媒体服务
  void CloseConnect();  // 关闭流媒体服务的连接
  int connect_count = 0;

  AVFormatContext* fmt_ctx = nullptr;

  // 视频帧
  AVCodecContext* video_codec_ctx = nullptr;
  AVStream* video_stream = nullptr;
  int video_index = -1;
  void AddVideoFrame(std::shared_ptr<AVFrame> frame);
  int GetVideoFrameQSize();
  void ClearVideoFrameQueue();

 public:
  static void EncodeVideoThread(void* arg);  // 编码视频帧并推流
  void EncodeVideo();

 private:
  Worker* m_worker_;

  // 视频帧
  ReaderWriterQueue<std::shared_ptr<AVFrame>> video_frame_q_;
  std::counting_semaphore<> video_frame_cs_{0};
  std::shared_ptr<AVFrame> PopVideoFrame();
};

}  // namespace avs_analyzer
#endif  // ANALYZER_AVPUSHSTREAM_H
