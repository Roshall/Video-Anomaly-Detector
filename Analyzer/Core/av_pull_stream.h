#ifndef ANALYZER_AVPULLSTREAM_H
#define ANALYZER_AVPULLSTREAM_H
#include <mutex>
#include <queue>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}
namespace avs_analyzer {
class Worker;
// TODO: support cache, and throw away data when cache is full,
//  consider using this queue lib:
//  https://github.com/cameron314/readerwriterqueue?tab=readme-ov-file it's
//  thread-safe and lock-free.
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
  bool GetVideoPkt(AVPacket& pkt,
                   int& pktQSize);  // 从队列获取的pkt，一定要主动释放!!!

  static void ReadThread(void* arg);  // 拉流媒体流
  void HandleRead();

 private:
  Worker* worker_;

  bool PushVideoPkt(const AVPacket& pkt);
  void ClearVideoPktQ();
  std::queue<AVPacket> video_pkt_queue_;
  std::mutex pkt_queue_mtx_;
};

}  // namespace avs_analyzer
#endif  // ANALYZER_AVPULLSTREAM_H