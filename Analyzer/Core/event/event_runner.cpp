#include "event/event_runner.h"

#include <utility>

#include "alarm/alarm_pipeline.h"

namespace avs_analyzer {

EventRunner::EventRunner(EventPipeline& events, AlarmPipeline& alarms,
                         bool notify_transition, Hooks hooks)
    : events_(events),
      alarms_(alarms),
      notify_transition_(notify_transition),
      hooks_(std::move(hooks)) {}

void EventRunner::Submit(const AVFrame* frame, const ConvertorState& convertor,
                         int64_t timestamp_ms,
                         const std::vector<MonitorRegistry::Ptr>& monitors) {
  events_.SubmitFrame(frame, convertor, timestamp_ms, monitors);
}

void EventRunner::Process(const AVFrame* frame, int64_t timestamp_ms,
                          int64_t timestamp,
                          const std::vector<MonitorRegistry::Ptr>& monitors) {
  auto result = events_.ProcessFrame(frame, timestamp_ms, timestamp, monitors);

  // Offline analysis has no stream state to transition (no admin stream
  // events, no HLS), so only live sessions notify.
  if (notify_transition_) {
    alarms_.OnTransition(result.transition, result.event_id, timestamp);
  }

  for (const auto& request : result.alarms) {
    if (hooks_.on_alarm) {
      hooks_.on_alarm(request);
    }
    alarms_.EmitImage(request);
  }

  if (result.flushed_event) {
    SubmitEvent(std::move(result.flushed_event.value()), result.event_id);
  }

  if (result.annotated_frame && hooks_.on_annotated_frame) {
    hooks_.on_annotated_frame(result.annotated_frame);
  }
}

void EventRunner::SubmitEvent(std::unique_ptr<Event> event,
                              int64_t event_id) {
  if (!event) {
    return;
  }
  const int64_t start_pts = event->start_timestamp;
  alarms_.SubmitEvent(std::move(event), event_id, start_pts);
}

}  // namespace avs_analyzer
