#include "worker.h"

#include <chrono>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "Utils/common.h"
#include "Utils/log.h"
#include "alarm/alarm_pipeline.h"
#include "alarm_sound.h"
#include "analyzer.h"
#include "analyzer_registration.h"
#include "config.h"
#include "event/event_pipeline.h"
#include "event/event_runner.h"
#include "monitor.h"
#include "offline/offline_pipeline.h"
#include "scheduler.h"
#include "stream/stream_session.h"

namespace avs_analyzer {

Worker::Worker(Scheduler* scheduler, std::unique_ptr<Monitor> monitor,
               const EventDetectorConfig& event_cfg, bool file)
    : scheduler_(scheduler), file_(file) {
  metrics_ = MetricsRegistry::Instance().RegisterWorker(
      monitor->video_code.empty() ? monitor->code : monitor->video_code);
  AddMonitor(std::move(monitor), event_cfg);
}

Worker::~Worker() {
  LOGI("worker[videoCode={}, streamUrl={}] shutting down.", video_code_,
       stream_url_);
  state_.store(false, std::memory_order_release);
  if (stream_) {
    stream_->Stop();
  }
  // Wake any detector waiting on inference before joining the detect thread.
  MonitorRegistry::WakeAnalyzers(monitors_.Snapshot());
  if (detect_thread_.joinable()) {
    detect_thread_.join();
  }
  MonitorRegistry::StopAnalyzers(monitors_.Snapshot());
  // Joins the pull/push threads before the pipelines they reference go away.
  stream_.reset();
  events_.reset();
  alarms_.reset();
  MetricsRegistry::Instance().UnregisterWorker(video_code_);
}

void Worker::AddMonitor(std::unique_ptr<Monitor> monitor,
                        const EventDetectorConfig& event_cfg) {
  std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>> monitors;
  monitors.emplace_back(std::move(monitor), event_cfg);
  AddMonitorsBatch(std::move(monitors));
}

void Worker::AddMonitorsBatch(
    std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>>&&
        monitors) {
  if (monitors.empty()) {
    return;
  }

  // Build the contexts first; the registry lock is only held for the insert.
  const VideoContext video_context =
      stream_ ? stream_->VideoContextSnapshot() : VideoContext{};
  std::vector<MonitorRegistry::Ptr> contexts;
  contexts.reserve(monitors.size());
  for (auto& [monitor, event_cfg] : monitors) {
    if (!monitor) {
      continue;
    }

    monitor->start_timestamp = getCurTimestamp();
    if (monitor->video_code.empty()) {
      monitor->video_code = video_code_.empty() ? monitor->code : video_code_;
    }

    auto ctx = std::make_shared<MonitorContext>();
    ctx->event_cfg = event_cfg;
    ctx->analyzer =
        std::shared_ptr<Analyzer>(CreateAnalyzer(monitor.get(),
                                                 scheduler_->GetConfig()));
    ctx->analyzer->SetMetrics(metrics_);
    const size_t depth = file_
                             ? static_cast<size_t>(
                                   scheduler_->GetConfig().offline_pipeline_depth)
                             : 1;
    ctx->registration =
        scheduler_->CreateAnalyzerRegistration(*ctx->analyzer, depth);
    ctx->analyzer->SetRegistration(ctx->registration);
    try {
      ctx->detector = std::make_shared<EventDetector>(event_cfg, *ctx->analyzer);
    } catch (const std::exception& ex) {
      LOGE("Failed to create event detector for monitor {}: {}",
           monitor->code, ex.what());
      // Continue without detector - analyzer can still work.
    }

    if (video_code_.empty()) {
      video_code_ = monitor->video_code;
    }
    if (stream_url_.empty()) {
      stream_url_ = monitor->stream_url;
    } else if (monitor->stream_url != stream_url_) {
      LOGW("Worker {} ignores monitor {} custom stream_url {}; using {}",
           video_code_, monitor->code, monitor->stream_url, stream_url_);
      monitor->stream_url = stream_url_;
    }
    if (push_stream_url_.empty() && monitor->push_stream) {
      push_stream_url_ = monitor->push_stream_url;
    } else if (monitor->push_stream &&
               monitor->push_stream_url != push_stream_url_) {
      LOGW("Worker {} ignores monitor {} custom push_stream_url {}; using {}",
           video_code_, monitor->code, monitor->push_stream_url,
           push_stream_url_);
      monitor->push_stream_url = push_stream_url_;
    }

    ctx->monitor = std::move(monitor);
    // Configure ROIs before the context becomes visible to the detect loop.
    MonitorRegistry::PrepareForStream(*ctx, video_context);
    contexts.push_back(std::move(ctx));
  }

  monitors_.AddBatch(std::move(contexts));

  EnsurePipelines();

  // Rebuild only once after all monitors are added.
  RebuildEventManager();
  SyncPushRequest();
  EnsureAnnotatorReady(true);
}

size_t Worker::num_task() const { return monitors_.Size(); }

void Worker::GetAllMonitor(std::vector<Monitor>& monitors) const {
  const auto snapshot = monitors_.GetAll();
  monitors.reserve(monitors.size() + snapshot.size());
  monitors.insert(monitors.end(), snapshot.begin(), snapshot.end());
}

void Worker::EnsurePipelines() {
  if (!events_) {
    events_ = std::make_unique<EventPipeline>(scheduler_, video_code_, id_,
                                              file_, metrics_);
  }
  if (!alarms_) {
    alarms_ =
        std::make_unique<AlarmPipeline>(scheduler_, video_code_, file_, timer_);
  }
}

bool Worker::start(std::string& msg) {
  auto settle_start = [this](bool ok) {
    if (!start_settled_.exchange(true, std::memory_order_acq_rel)) {
      start_promise_.set_value(ok);
    }
  };

  EnsurePipelines();

  StreamSession::Options options;
  options.scheduler = scheduler_;
  options.video_code = video_code_;
  options.stream_url = stream_url_;
  options.push_stream_url = push_stream_url_;
  options.file = file_;
  options.state = &state_;
  options.timer = &timer_;
  options.metrics = metrics_;
  options.on_stream_error = [this]() { Remove(); };
  options.on_stream_opened = [this](const VideoContext& context) {
    OnStreamOpened(context);
  };

  stream_ = std::make_unique<StreamSession>(std::move(options));
  stream_->SetPushRequested(monitors_.AnyPushStream(), push_stream_url_);
  if (!stream_->Open(msg)) {
    stream_.reset();
    settle_start(false);
    return false;
  }

  if (const auto resources = stream_->Current()) {
    timer_.init(resources->fmt.get(), resources->video_stream);
  }
  events_->UpdateConfig(monitors_.Snapshot());

  state_.store(true, std::memory_order_release);
  stream_->StartThreads();
  detect_thread_ = std::thread([this]() { DetectEvent(); });
  settle_start(true);
  return true;
}

bool Worker::WaitStarted(std::chrono::milliseconds timeout) const {
  if (started_.wait_for(timeout) != std::future_status::ready) {
    return false;
  }
  return started_.get();
}

bool Worker::State() const { return state_.load(std::memory_order_acquire); }

void Worker::Remove() {
  state_.store(false, std::memory_order_release);
  if (alarms_) {
    alarms_->DeactivateStream();
  }
  if (stream_) {
    stream_->Stop();
  }
  // Only evict ourselves: a replacement worker may already hold this video
  // code if a new add raced with this one being stopped.
  scheduler_->RemoveWorker(video_code_, this);
}

void Worker::Stop() {
  state_.store(false, std::memory_order_release);
  if (alarms_) {
    alarms_->DeactivateStream();
  }
  if (stream_) {
    stream_->Stop();
  }
}

void Worker::OfflineAnalyze() {
  // The constructor's AddMonitor path built the pipelines; file workers never
  // call start(), so this is their entry point into the shared pipelines.
  OfflinePipeline::Run({scheduler_, video_code_, &monitors_, events_.get(),
                        alarms_.get(), &timer_});
}

void Worker::DetectEvent() {
  if (!stream_) {
    LOGW("decode video without active pull stream");
    return;
  }
  const auto initial = stream_->Current();
  if (!initial) {
    LOGW("decode video without open stream resources");
    return;
  }

  alarms_->Configure(initial->codec->width, initial->codec->height,
                     stream_->Fps(), stream_->PushWidth(),
                     stream_->PushHeight());
  EnsureAnnotatorReady();
  RebuildEventManager();

  EventRunner runner(
      *events_, *alarms_, /*notify_transition=*/true,
      EventRunner::Hooks{
          .on_alarm =
              [this](const AlarmRequest& request) {
                if (auto* alarm = scheduler_->GetAlarmController()) {
                  alarm->trigger();
                  LOGD("Alarm sound triggered for monitor {}",
                       request.monitor_code);
                }
              },
          .on_annotated_frame =
              [this](std::shared_ptr<const Frame> frame) {
                stream_->PushFrame(std::move(frame));
              },
      });

  DecodeLoopHandlers handlers;
  handlers.on_frame = [this, &runner](const DecodedFrame& decoded) {
    return HandleFrame(runner, decoded);
  };
  handlers.on_new_generation = [this]() {
    // A reconnect may change the geometry; rebuild the annotator for the
    // newly published video context before annotating this generation.
    EnsureAnnotatorReady(true);
  };
  handlers.on_geometry_changed = [this](int width, int height, int fps,
                                        int out_width, int out_height) {
    alarms_->OnGeometryChanged(width, height, fps, out_width, out_height,
                               events_->CurrentEventId());
  };
  handlers.on_error_budget_exhausted = [this]() { Remove(); };
  stream_->RunDecodeLoop(handlers);

  const bool had_active = events_->HasActiveEvent();
  const int64_t tail_id = events_->CurrentEventId();
  auto tail_events = events_->FlushPending();
  alarms_->FinalizeOnStreamEnd(
      had_active, std::move(tail_events), tail_id,
      canceled_all_monitors_.load(std::memory_order_acquire));
}

bool Worker::HandleFrame(EventRunner& runner, const DecodedFrame& decoded) {
  // One snapshot per frame: admission and event processing must see the same
  // monitor set.
  const auto snapshot = monitors_.Snapshot();
  const auto convertor = stream_->Convertor();
  if (!convertor) {
    LOGW("frame submit skipped: stream convertor unavailable");
    if (metrics_) {
      metrics_->frames_skipped.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    runner.Submit(decoded.frame, *convertor, decoded.timestamp_ms, snapshot);
  }

  runner.Process(decoded.frame, decoded.timestamp_ms, decoded.timestamp,
                 snapshot);
  return State();
}

bool Worker::ContainsMonitor(const std::string& code) const {
  return monitors_.Contains(code);
}

void Worker::RemoveMonitor(const std::string& code) {
  std::vector<std::string> codes;
  codes.push_back(code);
  RemoveMonitorsBatch(codes);
}

void Worker::RemoveMonitorsBatch(const std::vector<std::string>& codes) {
  if (codes.empty()) {
    return;
  }

  // The registry marks contexts inactive and erases them under its lock; the
  // blocking Stop()/deregistration work runs outside so the detect loop is
  // never blocked by an in-flight inference.
  const auto removed = monitors_.RemoveBatch(codes);
  MonitorRegistry::StopAnalyzers(removed);
  if (removed.empty()) {
    return;
  }

  if (monitors_.Empty()) {
    canceled_all_monitors_.store(true, std::memory_order_release);
    Remove();
    return;
  }

  SyncPushRequest();
  RebuildEventManager();
  // Rebuild the annotator with the updated legend.
  EnsureAnnotatorReady(true);
}

void Worker::EnsureAnnotatorReady(bool force_rebuild) {
  if (!events_) {
    return;
  }
  const VideoContext video_context =
      stream_ ? stream_->VideoContextSnapshot() : VideoContext{};
  if (video_context.width <= 0 || video_context.height <= 0 ||
      video_context.format == AV_PIX_FMT_NONE) {
    return;
  }
  events_->EnsureAnnotator(video_context, monitors_.TaskLabels(),
                            force_rebuild);
}

void Worker::RebuildEventManager() {
  if (events_) {
    events_->UpdateConfig(monitors_.Snapshot());
  }
}

void Worker::SyncPushRequest() {
  if (!stream_) {
    return;
  }
  stream_->SetPushRequested(monitors_.AnyPushStream(), push_stream_url_);
}

void Worker::OnStreamOpened(const VideoContext& context) {
  // Runs on the pull thread before the first frame of the new generation is
  // published; only the cheap ROI reparse belongs here. The annotator is
  // rebuilt by the detect thread when it observes the new generation.
  monitors_.PrepareForStream(context);
}

uint64_t Worker::NextId() {
  static std::atomic<uint64_t> global_id = 0;
  return global_id.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace avs_analyzer
