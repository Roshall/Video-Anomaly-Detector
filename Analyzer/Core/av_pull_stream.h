#ifndef ANALYZER_AVPULLSTREAM_H
#define ANALYZER_AVPULLSTREAM_H
#include <concurrentqueue/moodycamel/concurrentqueue.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <semaphore>

#include "video_processing/helper.h"
#include "video_processing/video_context.h"

extern "C" {
#include "libavformat/avformat.h"
}
namespace avs_analyzer {
using namespace moodycamel;
class Worker;

// One immutable set of demux/decode resources. Reconnect publishes a new
// instance; consumers keep the one they are using alive via shared_ptr.
struct StreamResources {
  AVFormatContextPtr fmt;
  AVCodecContextPtr codec;
  AVStream* video_stream{nullptr};
  VideoContext ctx;
  AVRational time_base{0, 1};
  int video_index{-1};
  uint64_t generation{0};
};

using StreamResourcesPtr = std::shared_ptr<StreamResources>;

struct StreamPacket {
  AVPacketPtr pkt;
  uint64_t generation{0};
};

class AvPullStream {
 public:
  explicit AvPullStream(Worker* worker);
  ~AvPullStream();
  AvPullStream(const AvPullStream&) = delete;
  AvPullStream operator=(const AvPullStream&) = delete;

  bool Connect(bool file = false);  // 连接流媒体服务
  void CloseConnect();

  std::atomic<int> connect_count{0};

  [[nodiscard]] StreamResourcesPtr Current() const {
    return std::atomic_load(&current_);
  }
  // Blocks until a packet is queued or the stream shuts down/ends.
  bool GetVideoPkt(StreamPacket& packet);
  [[nodiscard]] bool Eof() const { return eof_.load(std::memory_order_acquire); }
  [[nodiscard]] bool Shutdown() const {
    return shutdown_.load(std::memory_order_acquire);
  }

  // 拉流媒体流
  void Read();
  void SignalShutdown();
  void ClearPacketQueue();

 private:
  bool OpenResources(bool file, StreamResourcesPtr& out, std::string& error);
  void PublishResources(StreamResourcesPtr resources);
  bool Reconnect();
  bool SleepInterruptible(int ms);

  Worker* worker_;
  std::shared_ptr<StreamResources> current_{};
  // MPMC: the detect thread consumes, while the read thread flushes stale
  // packets on reconnect (and CloseConnect may flush during shutdown).
  ConcurrentQueue<StreamPacket> video_pkt_queue_;
  std::counting_semaphore<> pkt_cs_{0};
  std::atomic<bool> shutdown_{false};
  std::atomic<bool> eof_{false};
  uint64_t generation_{0};
  // File inputs are reopened as files, never with RTSP options.
  bool file_{false};
  // Protects OpenResources so Connect() and Read() cannot interleave.
  std::mutex open_mutex_;
};

}  // namespace avs_analyzer
#endif  // ANALYZER_AVPULLSTREAM_H
