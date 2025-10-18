#ifndef ANALYZER_WORKER_H
#define ANALYZER_WORKER_H
#include <readerwriterqueue/readerwriterqueue.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "event.h"
#include "frame_walk_time.h"
#include "helper.h"
#include "jpeg_encoder.h"

namespace avs_analyzer {
class Scheduler;
class AvPullStream;
class AvPushStream;
class Analyzer;
struct Monitor;
struct Frame;
using namespace moodycamel;

class Worker {
 public:
  explicit Worker(Scheduler* scheduler, std::unique_ptr<Monitor> control);
  ~Worker();
  Worker(const Worker& other) = delete;
  Worker operator= (const Worker& other) = delete;

  // （线程）解码视频帧和实时分析视频帧
  void DecodeVideo();

  bool start(std::string& msg);

  bool getState() const;
  void remove();

  std::unique_ptr<Monitor> monitor_;
  Scheduler* scheduler;
  std::unique_ptr<AvPullStream> in_stream{};
  std::unique_ptr<AvPushStream> out_stream{};
  std::unique_ptr<Analyzer> analyzer{};

 private:
  FrameWallClock timer_;
  std::atomic<bool> state_{false};
  std::vector<std::unique_ptr<std::thread>> threads_;

  bool GrabFrame(AVFrame* frame) const;
  void AlarmImage(EventDetector& checker, JPEGEncoder& pict_writer,
                   FrameWallClock::sys_time start_ts, bool push) noexcept;
  void OfflineAnalyze();
};
}  // namespace avs_analyzer
#endif  // ANALYZER_WORKER_H
