#include "worker.h"
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Utils/common.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "admission.h"
#include "alarm/alarm_pipeline.h"
#include "alarm_sound.h"
#include "analyzer.h"
#include "analyzer_registration.h"
#include "config.h"
#include "event/event_pipeline.h"
#include "frame.h"
#include "helper.h"
#include "monitor.h"
#include "scheduler.h"
#include "stream/stream_session.h"
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
  WakeAnalyzers(monitors_.Snapshot());
  if (detect_thread_.joinable()) {
    detect_thread_.join();
  }
  StopAnalyzers(monitors_.Snapshot());
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
    PrepareMonitorForStream(*ctx, video_context);
    contexts.push_back(std::move(ctx));
  }

  monitors_.AddBatch(std::move(contexts));

  // The pipelines take their identity from the worker, so they can only be
  // built once the first monitor has established the video code.
  if (!events_) {
    events_ =
        std::make_unique<EventPipeline>(scheduler_, video_code_, file_, metrics_);
  }
  if (!alarms_) {
    alarms_ =
        std::make_unique<AlarmPipeline>(scheduler_, video_code_, file_, timer_);
  }

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

bool Worker::start(std::string& msg) {
  auto settle_start = [this](bool ok) {
    if (!start_settled_.exchange(true, std::memory_order_acq_rel)) {
      start_promise_.set_value(ok);
    }
  };

  if (!events_) {
    events_ =
        std::make_unique<EventPipeline>(scheduler_, video_code_, file_, metrics_);
  }
  if (!alarms_) {
    alarms_ =
        std::make_unique<AlarmPipeline>(scheduler_, video_code_, file_, timer_);
  }

  StreamSession::Options options;
  options.scheduler = scheduler_;
  options.video_code = video_code_;
  options.stream_url = stream_url_;
  options.push_stream_url = push_stream_url_;
  options.file = file_;
  options.state = &state_;
  options.metrics = metrics_;
  options.on_stream_error = [this]() { Remove(); };
  options.on_stream_opened = [this](const VideoContext& context) {
    OnStreamOpened(context);
  };

  stream_ = std::make_unique<StreamSession>(std::move(options));
  stream_->SetPushRequested(NeedPushStream(), push_stream_url_);
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

void Worker::WakeAnalyzers(
    const std::vector<MonitorContextPtr>& contexts) const {
  for (const auto& ctx : contexts) {
    if (ctx && ctx->analyzer) {
      // Wakes a detector blocked on the next inference result.
      ctx->analyzer->Stop();
    }
  }
}

void Worker::StopAnalyzers(
    const std::vector<MonitorContextPtr>& contexts) const {
  for (const auto& ctx : contexts) {
    if (!ctx || !ctx->analyzer) {
      continue;
    }
    ctx->analyzer->Stop();
    // Resetting the registration releases the mailbox; the analyzer also
    // holds a reference, so clear both.
    ctx->analyzer->SetRegistration(nullptr);
    ctx->registration.reset();
  }
}

void Worker::OfflineAnalyze() {
  const auto monitors = monitors_.Snapshot();
  if (monitors.empty()) {
    LOGW("offline analyze skipped: worker {} has no monitors", video_code_);
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
                      scheduler_->GetConfig().admin_host, code);
      // Never block the media pool on the admin service.
      g_control_pool.detach_task([url, code]() {
        std::string res;
        Request::get(url.c_str(), res);
        LOGI("offline analyzer response ({}): {}", code, res);
      });
    }
    StopAnalyzers(monitors);
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

  auto offline_convertor = std::make_shared<ConvertorState>();
  offline_convertor->width = width;
  offline_convertor->height = height;
  offline_convertor->format = in.PixelFormat();
  offline_convertor->convertor = std::make_shared<FormatConvertor>(
      width, height, in.PixelFormat(), width, height);
  offline_convertor_ =
      std::shared_ptr<const ConvertorState>(std::move(offline_convertor));

  auto in_stream_ = in.stream();
  timer_.init(in.format_context(), in_stream_, true);

  VideoContext overlay_ctx{.time_base = in_time_base,
                           .sample_aspect_ratio = AVRational{1, 1},
                           .format = static_cast<AVPixelFormat>(example->format),
                           .width = width,
                           .height = height,
                           .video_index = -1};
  for (const auto& ctx : monitors) {
    PrepareMonitorForStream(*ctx, overlay_ctx);
  }

  events_->UpdateConfig(monitors, true);
  events_->EnsureAnnotator(overlay_ctx, GetMonitorTasks(), true);
  alarms_->Configure(width, height, 25, width, height);

  auto submit_event = [&](std::unique_ptr<Event> event, int64_t alarm_id) {
    if (!event) {
      return;
    }
    const int64_t start_pts = event->start_timestamp;
    alarms_->SubmitEvent(std::move(event), alarm_id, start_pts);
  };

  // Submit frames up to `pipeline_depth` ahead of the event-processing
  // cursor so inference and event handling overlap. The mailbox backlog per
  // analyzer is bounded by the same depth.
  struct QueuedFrame {
    AVFramePtr frame;
    int64_t timestamp_ms{0};
    int64_t timestamp{0};
  };
  std::deque<QueuedFrame> pending;
  const size_t pipeline_depth = static_cast<size_t>(
      std::max(1, scheduler_->GetConfig().offline_pipeline_depth));

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
    AddTasks(holder.get(), in_time_base);
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

    auto result = events_->ProcessFrame(current.frame.get(), current.timestamp_ms,
                                        current.timestamp, monitors);
    for (const auto& request : result.alarms) {
      alarms_->EmitImage(request);
    }
    if (result.flushed_event) {
      submit_event(std::move(result.flushed_event.value()), result.event_id);
    }
  }

  // Flush any remaining pending events at EOF.
  const int64_t tail_id = events_->CurrentEventId();
  for (auto& event : events_->FlushPending()) {
    submit_event(std::move(event), tail_id);
  }

  finish();
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

  int width = initial->codec->width;
  int height = initial->codec->height;
  uint64_t current_generation = initial->generation;
  int fps = stream_->Fps();
  int out_width = stream_->PushWidth();
  int out_height = stream_->PushHeight();

  AVFramePtr frame{av_frame_alloc()};
  if (!frame) {
    LOGE("av_frame_alloc failed");
    return;
  }

  alarms_->Configure(width, height, fps, out_width, out_height);
  EnsureAnnotatorReady();
  RebuildEventManager();

  int consecutive_errors = 0;
  bool stream_done = false;

  while (State() && !stream_done) {
    StreamResourcesPtr frame_resources;
    switch (stream_->GrabFrame(frame.get(), frame_resources)) {
      case GrabResult::kOk:
        consecutive_errors = 0;
        AddTasks(frame.get(), frame->time_base,
                 frame_resources ? frame_resources->codec->framerate
                                 : AVRational{0, 1});
        break;
      case GrabResult::kAgain:
      case GrabResult::kDecodeError: {
        if (++consecutive_errors >= 50) {
          LOGE("worker {} exceeded decode error budget, removing worker",
               video_code_);
          Remove();
          stream_done = true;
          break;
        }
        const int delay_ms = std::min(500, 10 * consecutive_errors);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        continue;
      }
      case GrabResult::kReconnecting:
        // A reconnect is in progress; wait briefly instead of spinning.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      case GrabResult::kEof:
        LOGI("worker {} reached end of stream", video_code_);
        stream_done = true;
        break;
      case GrabResult::kFatal:
        stream_done = true;
        break;
    }
    if (stream_done) {
      break;
    }
    if (metrics_) {
      metrics_->frames_decoded.fetch_add(1, std::memory_order_relaxed);
    }

    if (frame_resources &&
        frame_resources->generation != current_generation) {
      current_generation = frame_resources->generation;
      timer_.init(frame_resources->fmt.get(), frame_resources->video_stream);
      last_frame_ts_ms_.store(-1, std::memory_order_relaxed);
      // A reconnect may change the geometry; rebuild the annotator for the
      // newly published video context before annotating this generation.
      EnsureAnnotatorReady(true);
    }

    const auto timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
      // AddTasks could not resolve a timestamp for this frame.
      av_frame_unref(frame.get());
      continue;
    }
    const AVRational frame_time_base = frame->time_base;
    const auto timestamp_ms =
        av_rescale_q(timestamp, frame_time_base, AVRational{1, 1000});

    // A reconnect may change the decoded geometry; rebuild per-stream output
    // resources before processing this frame.
    if (frame_resources &&
        (frame_resources->codec->width != width ||
         frame_resources->codec->height != height)) {
      width = frame_resources->codec->width;
      height = frame_resources->codec->height;
      fps = stream_->Fps();
      out_width = stream_->PushWidth();
      out_height = stream_->PushHeight();
      alarms_->OnGeometryChanged(width, height, fps, out_width, out_height,
                                 events_->CurrentEventId());
    }

    auto result = events_->ProcessFrame(frame.get(), timestamp_ms, timestamp,
                                        monitors_.Snapshot());
    alarms_->OnTransition(result.transition, result.event_id, timestamp);

    for (const auto& request : result.alarms) {
      if (auto* alarm = scheduler_->GetAlarmController()) {
        alarm->trigger();
        LOGD("Alarm sound triggered for monitor {}", request.monitor_code);
      }
      alarms_->EmitImage(request);
    }

    if (result.flushed_event) {
      auto& event = result.flushed_event.value();
      if (event) {
        const int64_t start_pts = event->start_timestamp;
        alarms_->SubmitEvent(std::move(event), result.event_id, start_pts);
      }
    }

    // Route annotated frames to the output stream.
    if (result.annotated_frame) {
      stream_->PushFrame(result.annotated_frame);
    }

    av_frame_unref(frame.get());
  }

  const bool had_active = events_->HasActiveEvent();
  const int64_t tail_id = events_->CurrentEventId();
  auto tail_events = events_->FlushPending();
  alarms_->FinalizeOnStreamEnd(
      had_active, std::move(tail_events), tail_id,
      canceled_all_monitors_.load(std::memory_order_acquire));
}

bool Worker::ResolveFrameTimestamps(AVFrame* frame, AVRational time_base,
                                    AVRational fps_hint, int64_t& timestamp,
                                    int64_t& timestamp_ms) const {
  timestamp = frame->best_effort_timestamp;
  if (timestamp == AV_NOPTS_VALUE) {
    timestamp = frame->pts;
  }
  if (timestamp != AV_NOPTS_VALUE) {
    timestamp_ms = av_rescale_q(timestamp, time_base, AVRational{1, 1000});
    last_frame_ts_ms_.store(timestamp_ms, std::memory_order_relaxed);
    return true;
  }

  AVRational fps = fps_hint;
  int64_t step_ms = 0;
  if (fps.num > 0 && fps.den > 0) {
    step_ms = std::llround(1000.0 * fps.den / fps.num);
  }
  if (step_ms <= 0) {
    if (!nopts_logged_.exchange(true, std::memory_order_relaxed)) {
      LOGW("worker {} cannot resolve frame timestamps (missing PTS and frame "
           "rate); dropping frames",
           video_code_);
    }
    return false;
  }
  const int64_t last = last_frame_ts_ms_.load(std::memory_order_relaxed);
  timestamp_ms = last >= 0 ? last + step_ms : 0;
  timestamp = av_rescale_q(timestamp_ms, AVRational{1, 1000}, time_base);
  // Keep the detect loop aligned with the synthesised timestamp.
  frame->best_effort_timestamp = timestamp;
  last_frame_ts_ms_.store(timestamp_ms, std::memory_order_relaxed);
  return true;
}

void Worker::AddTasks(AVFrame* frame, AVRational time_base,
                      AVRational fps_hint) const {
  if (!frame) {
    return;
  }
  const auto state = stream_ ? stream_->Convertor() : offline_convertor_;
  if (!state || !state->convertor) {
    LOGW("worker AddTasks skipped: convertor not initialized");
    if (metrics_) {
      metrics_->frames_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  // Frames decoded from a previous generation may still arrive right after a
  // reconnect; skip them rather than feeding a mismatched convertor.
  if (frame->width != state->width || frame->height != state->height ||
      frame->format != state->format) {
    if (metrics_) {
      metrics_->frames_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }

  int64_t timestamp = 0;
  int64_t timestamp_ms = 0;
  if (!ResolveFrameTimestamps(frame, time_base, fps_hint, timestamp,
                              timestamp_ms)) {
    if (metrics_) {
      metrics_->frames_skipped.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }

  const auto snapshot = monitors_.Snapshot();
  if (snapshot.empty()) {
    return;
  }

  // Admission control: only submit frames each detector actually wants, and
  // group monitors of the same task so they share one inference.
  std::vector<AdmissionEntry> entries;
  entries.reserve(snapshot.size());
  std::unordered_map<unsigned long, MonitorContextPtr> by_id;
  by_id.reserve(snapshot.size());
  for (const auto& ctx : snapshot) {
    if (!ctx || !ctx->monitor || !ctx->analyzer || !ctx->detector) {
      continue;
    }
    if (!ctx->active.load(std::memory_order_acquire)) {
      continue;
    }
    const unsigned long analyzer_id = ctx->analyzer->id();
    entries.push_back(
        {analyzer_id, ctx->monitor->task, ctx->analyzer->WantsFrame()});
    by_id.emplace(analyzer_id, ctx);
  }
  if (entries.empty()) {
    return;
  }

  const auto admission = AdmitFrame(entries, static_cast<uint64_t>(timestamp_ms));
  if (admission.skipped > 0 && metrics_) {
    metrics_->frames_skipped.fetch_add(admission.skipped,
                                       std::memory_order_relaxed);
  }
  if (admission.groups.empty()) {
    return;
  }

  // FormatConvertor returns a view over a reusable internal buffer.
  // Convert and clone once per decoded frame; every admitted task group
  // shares the same read-only image.
  const cv::Mat img_view = (*state->convertor)(frame);
  auto img = std::make_shared<const cv::Mat>(img_view.clone());

  size_t submitted = 0;
  for (const auto& group : admission.groups) {
    for (const auto analyzer_id : group.analyzer_ids) {
      const auto it = by_id.find(analyzer_id);
      if (it == by_id.end()) {
        continue;
      }
      it->second->analyzer->AddVideoFrame(id_, timestamp_ms, img, group.shared);
      ++submitted;
    }
  }
  if (metrics_ && submitted > 0) {
    metrics_->frames_submitted.fetch_add(submitted, std::memory_order_relaxed);
  }
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
  StopAnalyzers(removed);
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

void Worker::PrepareMonitorForStream(
    MonitorContext& ctx, const VideoContext& video_context) const {
  if (!ctx.monitor || !ctx.analyzer) {
    return;
  }
  if (video_context.width <= 0 || video_context.height <= 0) {
    return;
  }
  auto& monitor = *ctx.monitor;
  if (!monitor.ParseRecognitionRegion(video_context.width,
                                      video_context.height)) {
    LOGE("ParseRecognitionRegion() error for monitor {}", monitor.code);
    return;
  }
  if (monitor.task == Task::kBunkOff) {
    ctx.analyzer->SetROIs(monitor.roi_polygons);
  } else {
    ctx.analyzer->SetROIs({monitor.roi_points});
  }
  ctx.roi_entries.clear();
  if (!monitor.roi_points.empty()) {
    ctx.roi_entries.emplace_back(monitor.task, monitor.roi_points);
  }
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
  events_->EnsureAnnotator(video_context, GetMonitorTasks(), force_rebuild);
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
  stream_->SetPushRequested(NeedPushStream(), push_stream_url_);
}

bool Worker::NeedPushStream() const {
  const auto snapshot = monitors_.Snapshot();
  return std::ranges::any_of(snapshot, [](const auto& ctx) {
    return ctx && ctx->monitor && ctx->monitor->push_stream;
  });
}

std::vector<std::pair<Task, std::u8string>> Worker::GetMonitorTasks() const {
  std::vector<std::pair<Task, std::u8string>> tasks;
  const auto snapshot = monitors_.Snapshot();
  tasks.reserve(snapshot.size());
  for (const auto& ctx : snapshot) {
    if (!ctx || !ctx->monitor) {
      continue;
    }
    const auto task = ctx->monitor->task;
    const auto it = std::find_if(tasks.begin(), tasks.end(),
                                 [task](const auto& entry) {
                                   return entry.first == task;
                                 });
    if (it == tasks.end()) {
      const auto label = ctx->monitor->behavior.empty()
                             ? toChinese(task)
                             : ctx->monitor->behavior;
      tasks.emplace_back(task, label);
    }
  }
  return tasks;
}

void Worker::OnStreamOpened(const VideoContext& context) {
  // Runs on the pull thread before the first frame of the new generation is
  // published; only the cheap ROI reparse belongs here. The annotator is
  // rebuilt by the detect thread when it observes the new generation.
  for (const auto& ctx : monitors_.Snapshot()) {
    PrepareMonitorForStream(*ctx, context);
  }
}

uint64_t Worker::NextId() {
  static std::atomic<uint64_t> global_id = 0;
  return global_id.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace avs_analyzer
