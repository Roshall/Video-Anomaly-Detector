#include "offline/offline_pipeline.h"

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

#include <spdlog/fmt/fmt.h>

#include "Utils/common.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "alarm/alarm_pipeline.h"
#include "config.h"
#include "event.h"
#include "event/event_pipeline.h"
#include "frame.h"
#include "frame_walk_time.h"
#include "helper.h"
#include "video_processing/format_convertor.h"
#include "monitor/monitor_registry.h"
#include "scheduler.h"
#include "streamer.h"

namespace avs_analyzer {
namespace {

// Allocates a reference-counted copy of a decoded frame for offline analysis.
AVFramePtr CloneFrame(const AVFrame* source) {
  if (!source) {
    return {};
  }
  AVFrame* holder = av_frame_alloc();
  if (!holder || av_frame_ref(holder, source) < 0) {
    av_frame_free(&holder);
    return {};
  }
  return AVFramePtr(holder);
}

}  // namespace

void OfflinePipeline::Run(Context context) {
  const auto monitors = context.monitors->Snapshot();
  if (monitors.empty()) {
    LOGW("offline analyze skipped: worker {} has no monitors",
         context.video_code);
    return;
  }

  // Notify the admin service and stop the detectors on every exit path.
  auto finish = [&]() {
    for (const auto& ctx : monitors) {
      if (!ctx || !ctx->monitor) {
        continue;
      }
      const std::string code = ctx->monitor->code;
      const std::string url =
          fmt::format("{}/api/offline/finished?controlCode={}",
                      context.scheduler->GetConfig().admin_host, code);
      // Never block the media pool on the admin service.
      g_control_pool.detach_task([url, code]() {
        std::string res;
        Request::get(url.c_str(), res);
        LOGI("offline analyzer response ({}): {}", code, res);
      });
    }
    MonitorRegistry::StopAnalyzers(monitors);
  };

  const std::string stream_url = monitors.front()->monitor->stream_url;

  auto in{Streamer(stream_url)};
  if (!in.Valid()) {
    LOGE("invalid stream");
    finish();
    return;
  }
  const int width = in.width();
  const int height = in.height();
  const auto in_time_base = in.time_base;
  const auto example = in.NextFrame();
  if (!example) {
    LOGE("empty frame.");
    finish();
    return;
  }
  const int64_t start_timestamp = example->best_effort_timestamp;

  auto convertor = std::make_shared<ConvertorState>();
  convertor->width = width;
  convertor->height = height;
  convertor->format = in.PixelFormat();
  convertor->convertor =
      std::make_shared<FormatConvertor>(width, height, in.PixelFormat(), width,
                                        height);

  if (context.timer) {
    context.timer->init(in.format_context(), in.stream(), true);
  }

  VideoContext overlay_ctx{.time_base = in_time_base,
                           .sample_aspect_ratio = AVRational{1, 1},
                           .format = static_cast<AVPixelFormat>(example->format),
                           .width = width,
                           .height = height,
                           .video_index = -1};
  for (const auto& ctx : monitors) {
    if (ctx) {
      MonitorRegistry::PrepareForStream(*ctx, overlay_ctx);
    }
  }

  context.events->UpdateConfig(monitors, true);
  context.events->EnsureAnnotator(overlay_ctx,
                                  context.monitors->TaskLabels(), true);
  context.alarms->Configure(width, height, 25, width, height);

  auto submit_event = [&](std::unique_ptr<Event> event, int64_t alarm_id) {
    if (!event) {
      return;
    }
    const int64_t start_pts = event->start_timestamp;
    context.alarms->SubmitEvent(std::move(event), alarm_id, start_pts);
  };

  // Submit frames up to pipeline_depth ahead of the event-processing
  // cursor so inference and event handling overlap. The mailbox backlog per
  // analyzer is bounded by the same depth.
  struct QueuedFrame {
    AVFramePtr frame;
    int64_t timestamp_ms{0};
    int64_t timestamp{0};
  };
  std::deque<QueuedFrame> pending;
  const size_t pipeline_depth = static_cast<size_t>(
      std::max(1, context.scheduler->GetConfig().offline_pipeline_depth));

  auto enqueue_next = [&]() -> bool {
    const AVFrame* raw = in.NextFrame();
    if (!raw) {
      return false;
    }
    AVFramePtr holder = CloneFrame(raw);
    if (!holder) {
      return false;
    }
    const auto timestamp = holder->best_effort_timestamp - start_timestamp;
    holder->best_effort_timestamp = timestamp;
    holder->time_base = in_time_base;
    holder->pict_type = AV_PICTURE_TYPE_NONE;
    const auto timestamp_ms =
        av_rescale_q(timestamp, in_time_base, AVRational{1, 1000});
    context.events->SubmitFrame(holder.get(), *convertor, timestamp_ms,
                                monitors);
    pending.push_back({std::move(holder), timestamp_ms, timestamp});
    return true;
  };

  for (size_t i = 0; i < pipeline_depth; ++i) {
    if (!enqueue_next()) {
      break;
    }
  }

  while (!pending.empty()) {
    QueuedFrame current = std::move(pending.front());
    pending.pop_front();
    // Keep the pipeline full while the current frame's results are consumed.
    if (!in.EoF()) {
      enqueue_next();
    }

    auto result = context.events->ProcessFrame(
        current.frame.get(), current.timestamp_ms, current.timestamp,
        monitors);
    for (const auto& request : result.alarms) {
      context.alarms->EmitImage(request);
    }
    if (result.flushed_event) {
      submit_event(std::move(result.flushed_event.value()), result.event_id);
    }
  }

  // Flush any remaining pending events at EOF.
  const int64_t tail_id = context.events->CurrentEventId();
  for (auto& event : context.events->FlushPending()) {
    submit_event(std::move(event), tail_id);
  }

  finish();
}

}  // namespace avs_analyzer
