#ifndef ANALYZER_AVPUSHSTREAM_H
#define ANALYZER_AVPUSHSTREAM_H
#include <mutex>
#include <queue>
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}
namespace avs_analyzer {
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
  void AddVideoFrame(Frame* frame);
  int GetVideoFrameQSize();
  void ClearVideoFrameQueue();

 public:
  static void EncodeVideoThread(void* arg);  // 编码视频帧并推流
  void EncodeVideo();

 private:
  Worker* m_worker_;

  // 视频帧
  std::queue<Frame*> video_frame_q_;
  std::mutex video_frame_q_mtx_;
  bool GetVideoFrame(Frame*& frame);

  // bgr24转yuv420p
  static unsigned char ClipValue(unsigned char x, unsigned char min_val,
                          unsigned char max_val);
  static bool Bgr24ToYuv420P(unsigned char* bgrBuf, int w, int h,
                      unsigned char* yuvBuf);
};

}  // namespace avs_analyzer
#endif  // ANALYZER_AVPUSHSTREAM_H
