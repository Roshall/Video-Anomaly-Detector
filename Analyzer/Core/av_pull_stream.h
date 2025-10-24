#ifndef ANALYZER_AVPULLSTREAM_H
#define ANALYZER_AVPULLSTREAM_H
#include <readerwriterqueue/readerwriterqueue.h>

#include <semaphore>
#include <queue>

#include "video_processing/helper.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}
namespace avs_analyzer {
using namespace moodycamel;
class Worker;
class AvPullStream {
 public:
  explicit AvPullStream(Worker* worker);
  ~AvPullStream();
  AvPullStream(const AvPullStream&) = delete;
  AvPullStream operator=(const AvPullStream&) = delete;

  bool Connect(bool file =false);       // 连接流媒体服务
  bool Reconnect();     // 重连流媒体服务
  void CloseConnect();

  int connect_count = 0;

  AVFormatContextPtr fmt_ctx;

  // 视频帧
  AVCodecContextPtr video_codec_ctx;
  AVStream* stream = nullptr;
  AVPacketPtr GetVideoPkt();
  float GetFps() const;

  // 拉流媒体流
  void Read();
  void SignalShutdown();
  void ClearPacketQueue();

 private:
  Worker* worker_;
  ReaderWriterQueue<AVPacketPtr> video_pkt_queue_;
  std::counting_semaphore<> pkt_cs_{0};
};

}  // namespace avs_analyzer
#endif  // ANALYZER_AVPULLSTREAM_H