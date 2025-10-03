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

  bool Connect();       // 连接流媒体服务
  bool Reconnect();     // 重连流媒体服务
  void CloseConnect();  // 关闭流媒体服务的连接

  int connect_count = 0;

  AVFormatContext* fmt_ctx = nullptr;

  // 视频帧
  AVCodecContext* video_codec_ctx = nullptr;
  AVStream* stream = nullptr;
  AVPacketPtr GetVideoPkt();
  AVPacket* PeakPkt();

  static void ReadThread(void* arg);  // 拉流媒体流
  void HandleRead();

 private:
  Worker* worker_;
  void ClearVideoPktQ();
  ReaderWriterQueue<AVPacketPtr> video_pkt_queue_;
  std::counting_semaphore<> pkt_cs_{0};
};

}  // namespace avs_analyzer
#endif  // ANALYZER_AVPULLSTREAM_H