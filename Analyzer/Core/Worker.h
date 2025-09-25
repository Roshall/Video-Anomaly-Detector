#ifndef ANALYZER_WORKER_H
#define ANALYZER_WORKER_H
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace avs_analyzer {
class Scheduler;
class AvPullStream;
class AvPushStream;
class Analyzer;
struct Monitor;
struct Frame;
class FramePool;

class Worker {
 public:
  explicit Worker(Scheduler* scheduler, std::unique_ptr<Monitor> control);
  ~Worker();

  static void decodeVideoThread(
      void* arg);  // （线程）解码视频帧和实时分析视频帧
  void handleDecodeVideo();
  static void generateAlarmThread(void* arg);  // （线程）实时准备报警视频帧
  void handleGenerateAlarm();

  bool start(std::string& msg);

  bool getState() const;
  void remove();

  std::unique_ptr<Monitor> monitor;
  Scheduler* scheduler;
  std::unique_ptr<AvPullStream> in_stream{};
  std::unique_ptr<AvPushStream> out_Stream{};
  std::unique_ptr<Analyzer> analyzer{};
  std::unique_ptr<FramePool> video_frame_pool;

 private:
  bool state_{};
  std::vector<std::unique_ptr<std::thread>> threads_;

  // 报警视频帧
  std::queue<Frame*> alarm_frame_queue_;
  std::mutex frame_queue_mtx_;
  void AddAlarmVideoFrameQ(Frame* frame);
  size_t GetAlarmVideoFrameQSize();
  bool GetAlarmVideoFrame(Frame*& frame);
  void ClearAlarmVideoFrameQ();
};
}  // namespace avs_analyzer
#endif  // ANALYZER_WORKER_H
