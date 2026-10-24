#ifndef ANALYZER_OFFLINE_PIPELINE_H
#define ANALYZER_OFFLINE_PIPELINE_H

#include <string>

namespace avs_analyzer {
class AlarmPipeline;
class EventPipeline;
class FrameWallClock;
class MonitorRegistry;
class Scheduler;

// One-shot file analysis: reads the monitor's file through Streamer, submits
// frames through the event pipeline with a bounded look-ahead, and finalises
// alarms at EOF. It shares the event/alarm pipelines with live detection and
// never touches the network on the calling thread.
class OfflinePipeline {
 public:
  struct Context {
    Scheduler* scheduler{nullptr};
    std::string video_code;
    MonitorRegistry* monitors{nullptr};
    EventPipeline* events{nullptr};
    AlarmPipeline* alarms{nullptr};
    FrameWallClock* timer{nullptr};
  };

  // Blocks until the file is consumed or a fatal error occurs. Notifies the
  // admin service and stops the analyzers on every exit path.
  static void Run(Context context);
};

}  // namespace avs_analyzer

#endif  // ANALYZER_OFFLINE_PIPELINE_H
