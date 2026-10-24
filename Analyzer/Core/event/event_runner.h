#ifndef ANALYZER_EVENT_RUNNER_H
#define ANALYZER_EVENT_RUNNER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "event/event_pipeline.h"

namespace avs_analyzer {
class AlarmPipeline;
struct ConvertorState;

// One session's per-frame event flow: submit a frame for inference, run the
// detections through the event manager, and dispatch the resulting alarm
// images and flushed events. Live and offline sessions share this; they
// differ only in whether stream transitions are notified (live) and in the
// optional hooks for the alarm sound and the push output (live).
class EventRunner {
 public:
  struct Hooks {
    // Called once per alarm request (live: play the alarm sound).
    std::function<void(const AlarmRequest&)> on_alarm;
    // Called with the annotated frame, when there is one (live: push output).
    std::function<void(std::shared_ptr<const Frame>)> on_annotated_frame;
  };

  EventRunner(EventPipeline& events, AlarmPipeline& alarms,
              bool notify_transition, Hooks hooks = {});

  // Submits one frame for inference (admission control + one convert/clone).
  void Submit(const AVFrame* frame, const ConvertorState& convertor,
              int64_t timestamp_ms,
              const std::vector<MonitorRegistry::Ptr>& monitors);

  // Runs the event pipeline for one decoded frame and dispatches its result
  // (transition, alarm images, flushed events, annotated frame).
  void Process(const AVFrame* frame, int64_t timestamp_ms, int64_t timestamp,
               const std::vector<MonitorRegistry::Ptr>& monitors);

  // Submits a flushed event to the alarm pipeline (also used for the tail
  // flush when a session ends).
  void SubmitEvent(std::unique_ptr<Event> event, int64_t event_id);

 private:
  EventPipeline& events_;
  AlarmPipeline& alarms_;
  bool notify_transition_;
  Hooks hooks_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_EVENT_RUNNER_H
