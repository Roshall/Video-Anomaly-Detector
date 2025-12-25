#include "worker.h"

#include <json/value.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Utils/common.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "alarm.h"
#include "analyzer.h"
#include "av_pull_stream.h"
#include "av_push_stream.h"
#include "config.h"
#include "event.h"
#include "format_convertor.h"
#include "jpeg_encoder.h"
#include "monitor.h"
#include "scheduler.h"
#include "streamer.h"

extern "C" {
#include <libswresample/swresample.h>
}

namespace avs_analyzer {

// Utility: collect the Task list from the current worker's monitors.
std::vector<Task> Worker::GetMonitorTasks() const {
  std::vector<Task> tasks;
  std::shared_lock lock(monitors_mutex_);
  tasks.reserve(monitors_.size());
  for (const auto& ctx : monitors_) {
    if (!ctx || !ctx->monitor) continue;
    const auto t = ctx->monitor->task;
    if (std::find(tasks.begin(), tasks.end(), t) == tasks.end()) {
      tasks.push_back(t);
    }
  }
  return tasks;
}

Worker::Worker(Scheduler* scheduler, std::unique_ptr<Monitor> monitor,
               const EventDetectorConfig& event_cfg, bool file)
  : scheduler(scheduler), file_{file} {
  AddMonitor(std::move(monitor), event_cfg);
}

Worker::~Worker() {
  LOGI("worker[videoCode={}, streamUrl={}] shutting down.", video_code_,
       stream_url_);
  state_.store(false);
  if (in_stream) {
    in_stream->SignalShutdown();
  }
  if (auto out = std::atomic_load(&out_stream)) {
    out->SignalShutdown();
  }

  std::vector<std::unique_ptr<std::thread>> threads_to_join;
  {
    std::lock_guard lock(threads_mutex_);
    threads_to_join.swap(threads_);
  }
  for (auto& thread : threads_to_join) {
    if (thread && thread->joinable()) {
      thread->join();
    }
  }
}

void Worker::AddMonitor(std::unique_ptr<Monitor> monitor,
                        const EventDetectorConfig& event_cfg) {
  if (!monitor) {
    return;
  }
  monitor->start_timestamp = getCurTimestamp();
  if (monitor->video_code.empty()) {
    monitor->video_code = video_code_.empty() ? monitor->code : video_code_;
  }

  auto ctx = std::make_unique<MonitorContext>();
  
  ctx->monitor = std::move(monitor);

  ctx->event_cfg = event_cfg;
  ctx->analyzer = std::make_unique<Analyzer>(
      ctx->monitor.get(), scheduler->GetConfig(),
      ctx->monitor->conf_threshold);

  if (auto out = std::atomic_load(&out_stream)) {
    try {
      ctx->detector = std::make_unique<EventDetector>(
          event_cfg, *ctx->analyzer,
          FormatConvertor{video_context_.width, video_context_.height,
                          video_context_.format, out->width(),
                          out->height()});
    } catch (const std::exception& ex) {
      LOGE("Failed to create event detector for monitor {}: {}",
           ctx->monitor->code, ex.what());
    }
  }
  ctx->current_highest_level = 0;
  {
    std::unique_lock lock(monitors_mutex_);
    if (stream_url_.empty()) {
      stream_url_ = ctx->monitor->stream_url;
    } else if (ctx->monitor->stream_url != stream_url_) {
      LOGW("Worker {} ignores monitor {} custom stream_url {}; using {}",
           video_code_, ctx->monitor->code, ctx->monitor->stream_url,
           stream_url_);
      ctx->monitor->stream_url = stream_url_;
    }
    if (push_stream_url_.empty() && monitor->push_stream) {
      push_stream_url_ = ctx->monitor->push_stream_url;
    } else if (monitor->push_stream &&
               monitor->push_stream_url != push_stream_url_) {
      LOGW("Worker {} ignores monitor {} custom push_stream_url {}; using {}",
           video_code_, ctx->monitor->code, ctx->monitor->push_stream_url,
           push_stream_url_);
      ctx->monitor->push_stream_url = push_stream_url_;
    }
    if (video_code_.empty()) {
      video_code_ = ctx->monitor->video_code;
    }
    if (video_context_.valid()) {
      PrepareMonitorForStream(*ctx);
    }
    monitors_.push_back(std::move(ctx));
  }
  
  RebuildEventManager();
  StartPushThreadIfNeeded();

  // Update annotator legend whenever monitors are added
  EnsureAnnotatorReady();
  std::scoped_lock lock(annotator_mutex_);
  if (annotator_) {
    annotator_->update_legend(GetMonitorTasks());
  }
}

size_t Worker::num_task() const {
  std::shared_lock lock(monitors_mutex_);
  return monitors_.size();
}

void Worker::GetAllMonitor(std::vector<Monitor>& monitors) const {
  std::shared_lock lock(monitors_mutex_);
  monitors.reserve(monitors.size() + monitors_.size());
  for (const auto& ctx : monitors_) {
    monitors.push_back(*ctx->monitor);
  }
}

bool Worker::start(std::string& msg) {
  in_stream = std::make_unique<AvPullStream>(this);
  if (!in_stream->Connect(file_)) {
    msg = "pull stream connect error";
    in_stream.reset();
    return false;
  }

  if (NeedPushStream()) {
    auto out = std::make_shared<AvPushStream>(this);
    if (!out->Connect()) {
      msg = "push stream connect error";
      return false;
    }
    std::atomic_store(&out_stream, std::move(out));
  }

  timer_.init(in_stream->fmt_ctx.get(), in_stream->stream);

  state_.store(true);
  {
    std::lock_guard lock(threads_mutex_);
    threads_.push_back(
        std::make_unique<std::thread>([this]() { this->in_stream->Read(); }));
    threads_.push_back(
        std::make_unique<std::thread>([this]() { this->DecodeVideo(); }));
  }

  if (auto out = std::atomic_load(&out_stream)) {
    EnsureAllPipelinesReady(out->width(), out->height());
    std::lock_guard lock(threads_mutex_);
    threads_.push_back(std::make_unique<std::thread>(
        [out]() { out->EncodeVideo(); }));
  }
  return true;
}

bool Worker::State() const { return state_.load(); }

void Worker::Remove() {
  state_.store(false);
  if (in_stream) {
    in_stream->SignalShutdown();
  }
  if (auto out = std::atomic_load(&out_stream)) {
    out->SignalShutdown();
  }
  scheduler->RemoveWorker(video_code_);
}

void Worker::Stop() { state_.store(false); }

void Worker::OfflineAnalyze() {
  MonitorContext* ctx = nullptr;
  {
    std::shared_lock lock(monitors_mutex_);
    if (!monitors_.empty()) {
      ctx = monitors_.front().get();
    }
  }
  if (!ctx) {
    LOGE("offline analyze without monitor context");
    return;
  }

  auto& monitor = *ctx->monitor;
  auto in{Streamer(monitor.stream_url)};
  if (!in.Valid()) {
    LOGE("invalid stream");
    return;
  }
  const int width = in.width();
  const int height = in.height();
  const auto in_time_base = in.time_base;
  const auto example = in.NextFrame();
  if (!example) {
    LOGE("empty frame.");
    return;
  }
  monitor.roi_points.clear();
  if (!monitor.ParseRecognitionRegion(video_context_.width,
                                      video_context_.height)) {
    LOGE("cannot parse recognition region for {}", monitor.code);
    return;
  }
  std::vector<DynamicAnnotator::ROIEntry> offline_rois;
  if (!monitor.roi_points.empty()) {
    offline_rois.emplace_back(monitor.task, monitor.roi_points);
  }

  auto analyzer_ptr = std::make_unique<Analyzer>(
      ctx->monitor.get(), scheduler->GetConfig(), monitor.conf_threshold);
  auto in_stream_ = in.stream();
  timer_.init(in.format_context(), in_stream_);
  EventDetector checker{ctx->event_cfg,
                        *analyzer_ptr,
                        {width, height, in.PixelFormat(), width, height}};
  CombinedEventManager event_manager;
  CombinedEventConfig cfg{};
  cfg.pre_event_buffer_ms = ctx->event_cfg.pre_event_buffer_ms;
  cfg.post_event_buffer_ms = ctx->event_cfg.post_event_buffer_ms;
  cfg.max_event_duration_ms = ctx->event_cfg.max_event_duration_ms;
  cfg.guard_window_ms = std::max(
      static_cast<int64_t>(ctx->event_cfg.minor_stride_ms) *
      ctx->event_cfg.window_size,
      ctx->event_cfg.latest_duration_ms);
  event_manager.UpdateConfig(cfg);

  VideoContext overlay_ctx{.time_base = in_time_base,
                           .sample_aspect_ratio = AVRational{1, 1},
                           .format = static_cast<AVPixelFormat>(example->
                             format),
                           .width = width,
                           .height = height,
                           .video_index = -1};
  std::unique_ptr<DynamicAnnotator> annotator;
  try {
    auto candidate = std::make_unique<DynamicAnnotator>();
    const int ret = candidate->init(overlay_ctx);
    if (ret < 0) {
      LOGE("offline analyze: failed to init annotator for {}: {}",
           monitor.code, ret);
    } else {
      candidate->update_legend(GetMonitorTasks());
      annotator = std::move(candidate);
    }
  } catch (const std::exception& ex) {
    LOGE("offline analyze: failed to init annotator for {}: {}", monitor.code,
         ex.what());
  }
  if (annotator) {
    event_manager.SetAnnotator(annotator.get());
  }

  auto pict_writer = std::make_shared<JPEGEncoder>(width, height);
  const auto& levels = monitor.levels.second;
  const std::string level = levels.empty() ? std::string{} : levels.front();

  while (!in.EoF()) {
    const auto frame = in.NextFrame();
    if (!frame) {
      break;
    }
    const auto timestamp = frame->best_effort_timestamp;
    frame->time_base = in_time_base;
    frame->pict_type = AV_PICTURE_TYPE_NONE;
    const auto timestamp_ms =
        av_rescale_q(timestamp, in_time_base, AVRational{1, 1000});

    const auto state = checker.ProcessFrame({timestamp_ms, frame});

    CombinedEventManager::DetectorFrameInfo info{
      .monitor_code = monitor.code,
      .task = monitor.task,
      .state = state,
      .detections = &checker.detections(),
      .rois = offline_rois.empty() ? nullptr : &offline_rois,
      .event_start_timestamp_ms = checker.event_start_timestamp(),
      .event_start_pts = checker.start_ts(),
    };

    std::vector<CombinedEventManager::DetectorFrameInfo> info_vec;
    info_vec.emplace_back(info);

    auto combined = event_manager.ProcessFrame(frame, timestamp_ms, timestamp,
                                               info_vec);
    Frame* annotated_frame = combined.annotated_frame.get();

    switch (state) {
      case EventDetector::kEventStart:
        if (annotated_frame) {
          AlarmImage(monitor.code, checker.start_ts(), checker.timestamp(),
                     *annotated_frame, pict_writer, level, false);
        }
        break;
      default:
        break;
    }

    if (combined.flushed_event) {
      if (auto& event = combined.flushed_event.value()) {
        const auto start_ts = timer_.compute(event->start_timestamp);
        const int w = width;
        const int h = height;
        g_th_pool.detach_task(
            [start_ts, code = monitor.code, event = std::move(event), w, h,
             upload_dir = scheduler->GetConfig().upload_dir]() mutable {
              Alarm alarm(start_ts, std::move(code), std::move(event), w, h);
              alarm.Finalize(upload_dir, "");
            });
      }
    }
  }

  for (auto& event : event_manager.FlushPending()) {
    if (!event) continue;
    const auto start_ts = timer_.compute(event->start_timestamp);
    const int w = width;
    const int h = height;
    g_th_pool.detach_task(
        [start_ts, code = monitor.code, event = std::move(event), w, h,
         upload_dir = scheduler->GetConfig().upload_dir]() mutable {
          Alarm alarm(start_ts, std::move(code), std::move(event), w, h);
          alarm.Finalize(upload_dir, "");
        });
  }

  const std::string url =
      std::format("{}/api/offline/finished?code={}",
                  scheduler->GetConfig().admin_host, monitor.code);
  std::string res;
  Request::get(url.c_str(), res);
  LOGI("offline analyzer response: {}", res);
}

void Worker::AlarmImage(const std::string& monitor_code, int64_t start_pts,
                        int64_t event_pts, const Frame& frame,
                        std::shared_ptr<JPEGEncoder> pict_writer, std::string_view level,
                        bool push) noexcept {
  if (!frame.data) {
    LOGW("AlarmImage skipped for monitor {}: missing frame data",
         monitor_code);
    return;
  }

  const auto event_timestamp = timer_.compute(event_pts);
  const auto start_timestamp = timer_.compute(start_pts);

  AVFramePtr frame_clone{av_frame_clone(frame.data.get())};
  if (!frame_clone) {
    LOGE("AlarmImage failed to clone frame for monitor {}", monitor_code);
    return;
  }
  frame_clone->best_effort_timestamp = frame.data->best_effort_timestamp;
  frame_clone->pts = frame.data->pts;
  frame_clone->time_base = frame.data->time_base;

  auto async_frame = std::make_unique<Frame>();
  async_frame->timestamp = frame.timestamp;
  async_frame->object_count = frame.object_count;
  async_frame->data = std::move(frame_clone);

  g_th_pool.detach_task([pict_writer, conf = scheduler->GetConfig(),
        code = monitor_code, start_timestamp, event_timestamp,
        level = std::string(level), push,
        frame = std::move(async_frame)]() mutable {
        auto img_path =
            std::format("{}/alarm/{}/{}", conf.upload_dir, code,
                        FrameWallClock::to_iso8601_utc(start_timestamp));
        std::filesystem::create_directories(std::filesystem::path(img_path));
        auto time_str = FrameWallClock::to_iso8601_utc(event_timestamp);
        auto img_path_str = std::format("{}/{}.jpg", img_path, time_str);
          if (const int ret = pict_writer->saveFrame(
            frame->data.get(), img_path_str);
          ret < 0) {
          LOGE_FFMPEG("failed to save picture", ret);
          return;
        }

        if (push) {
          const std::string url =
              std::format("{}/api/postAddAlarm", conf.admin_host);
          Json::Value param;
          param["control_code"] = code;
          param["desc"] = "";
          param["img_path"] = img_path_str;
          param["create_time"] = time_str;
          param["level"] = level;

          const std::string data = param.toStyledString();
          std::string response;
          Request::post(url.data(), data.data(), response);
          LOGI("sent alarm image to {}", conf.admin_host);
        }
      });
}

bool Worker::NeedPushStream() const {
  std::shared_lock lock(monitors_mutex_);
  return std::ranges::any_of(
      monitors_, [](const auto& ctx) { return ctx->monitor->push_stream; });
}

CombinedEventConfig Worker::ComputeCombinedEventConfig() const {
  CombinedEventConfig cfg{};
  std::shared_lock lock(monitors_mutex_);
  for (const auto& ctx : monitors_) {
    const auto& event_cfg = ctx->event_cfg;
    cfg.pre_event_buffer_ms =
        std::max(cfg.pre_event_buffer_ms, event_cfg.pre_event_buffer_ms);
    cfg.post_event_buffer_ms =
        std::max(cfg.post_event_buffer_ms, event_cfg.post_event_buffer_ms);
    cfg.max_event_duration_ms =
        std::max(cfg.max_event_duration_ms, event_cfg.max_event_duration_ms);
    const int64_t guard = std::max(
        static_cast<int64_t>(event_cfg.minor_stride_ms) * event_cfg.window_size,
        event_cfg.latest_duration_ms);
    cfg.guard_window_ms = std::max(cfg.guard_window_ms, guard);
  }
  return cfg;
}

void Worker::RebuildEventManager() {
  auto cfg = ComputeCombinedEventConfig();
  std::scoped_lock lock(event_manager_mutex_, annotator_mutex_);
  if (!event_manager_) {
    event_manager_ = std::make_unique<CombinedEventManager>();
    event_manager_->SetAnnotatorMutex(&annotator_mutex_);
    event_manager_->SetAnnotator(annotator_.get());
  }
  event_manager_->UpdateConfig(cfg);
}

void Worker::EnsureAnnotatorReady() {
  if (!video_context_.valid() || video_context_.format == AV_PIX_FMT_NONE) {
    return;
  }
  std::scoped_lock lock(annotator_mutex_);
  // Quick-path: avoid reinitializing if it already exists
  if (annotator_) return;

  std::unique_ptr<DynamicAnnotator> candidate;
  try {
    candidate = std::make_unique<DynamicAnnotator>();
    if (const int ret = candidate->init(video_context_); ret < 0) {
      LOGE("Failed to initialize worker annotator ({}x{}, fmt={}): {}",
          video_context_.width, video_context_.height,
          static_cast<int>(video_context_.format), ret);
      return;
    }
    // Build legend from monitors snapshot
    candidate->update_legend(GetMonitorTasks());
  } catch (const std::exception& ex) {
    LOGE("Failed to initialize worker annotator: {}", ex.what());
    candidate.reset();
    return;
  }

// Set annotator and the event_manager's pointer under both locks to avoid races
  std::scoped_lock event_lock(event_manager_mutex_);
  annotator_ = std::move(candidate);
  if (event_manager_ && !event_manager_->HasAnnotator()) {
    event_manager_->SetAnnotator(annotator_.get());
  }
}

int Worker::ComputeLevelIndex(
    const std::pair<std::vector<int>, std::vector<std::string>>& levels,
    LevelMode level_mode, int64_t num_detection, int64_t event_duration,
    std::string_view monitor_code) {
  if (levels.first.empty() || levels.second.empty()) {
    LOGE("monitor {} has invalid/empty levels configuration", monitor_code);
    return 0;  // Always return lowest level (0) to avoid missing alarms
  }
  
  int64_t metric = level_mode == LevelMode::kNumPeople
                       ? num_detection
                       : event_duration;
  int idx = 0;
  const int total = static_cast<int>(levels.first.size());
  while (idx < total && levels.first[idx] <= metric) {
    ++idx;
  }
  --idx;
  if (idx < 0) {
    LOGW("monitor {} metric {} below first threshold {}", monitor_code,
         metric, levels.first.front());
    idx = 0;
  }
  idx = std::min(idx, static_cast<int>(levels.second.size()) - 1);
  return idx;
}

void Worker::DecodeVideo() {
  if (!in_stream || !in_stream->video_codec_ctx) {
    LOGW("decode video without active pull stream");
    return;
  }

  const int width = in_stream->video_codec_ctx->width;
  const int height = in_stream->video_codec_ctx->height;
  const auto in_time_base = in_stream->stream->time_base;

  AVFramePtr frame_yuv420p{av_frame_alloc()};
  if (!frame_yuv420p) {
    LOGE("av_frame_alloc failed");
    return;
  }

  int out_width = 0;
  int out_height = 0;
  if (auto out = std::atomic_load(&out_stream)) {
    out_width = out->width();
    out_height = out->height();
  } else {
    out_width = width;
    out_height = height;
  }

  auto pict_writer = std::make_shared<JPEGEncoder>(width, height);

  EnsureAnnotatorReady();
  RebuildEventManager();

  while (State()) {
    if (!GrabFrame(frame_yuv420p.get())) {
      continue;
    }

    const auto timestamp = frame_yuv420p->best_effort_timestamp;
    frame_yuv420p->time_base = in_time_base;
    frame_yuv420p->pict_type = AV_PICTURE_TYPE_NONE;
    const auto timestamp_ms =
        av_rescale_q(timestamp, in_time_base, AVRational{1, 1000});

    if (auto out = std::atomic_load(&out_stream)) {
      out_width = out->width();
      out_height = out->height();
    } else {
      out_width = width;
      out_height = height;
    }

    std::vector<CombinedEventManager::DetectorFrameInfo> detector_infos;
    std::vector<MonitorEventSnapshot> event_snapshots;

    {
      std::shared_lock lock(monitors_mutex_);
      detector_infos.reserve(monitors_.size());
      event_snapshots.reserve(monitors_.size());

      for (auto& ctx : monitors_) {
        auto* ctx_ptr = ctx.get();
        if (!ctx_ptr || !ctx_ptr->detector) {
          continue;
        }
        const auto state =
            ctx_ptr->detector->ProcessFrame({timestamp_ms, frame_yuv420p.get()});

        CombinedEventManager::DetectorFrameInfo info{
          .monitor_code = ctx_ptr->monitor->code,
          .task = ctx_ptr->monitor->task,
          .state = state,
          .detections = &ctx_ptr->detector->detections(),
          .rois = ctx_ptr->roi_entries.empty() ? nullptr :
              &ctx_ptr->roi_entries,
          .event_start_timestamp_ms = ctx_ptr->detector->event_start_timestamp(),
          .event_start_pts = ctx_ptr->detector->start_ts(),
        };
        detector_infos.push_back(info);

        // Capture snapshot for alarm processing (no pointers, thread-safe)
        event_snapshots.emplace_back(
        ctx_ptr->monitor->code,
        state,
        ctx_ptr->detector->start_ts(),
        ctx_ptr->detector->timestamp(),
        ctx_ptr->detector->num_detection(),
        ctx_ptr->detector->event_duration(),
        ctx_ptr->monitor->levels,
        ctx_ptr->monitor->level_mode,
        ctx_ptr->current_highest_level);
      }
    }
    // Lock released here - safe to process snapshots

    CombinedEventManager::Result combined_result;
    {
      std::lock_guard lock(event_manager_mutex_);
      if (event_manager_) {
        combined_result = event_manager_->ProcessFrame(
            frame_yuv420p.get(), timestamp_ms, timestamp, detector_infos);
      }
    }

    Frame* annotated_frame = combined_result.annotated_frame.get();

    // Process alarms using snapshots (no data race)
    for (auto& snapshot : event_snapshots) {
      auto level_idx = ComputeLevelIndex(
          snapshot.levels, snapshot.level_mode,
          snapshot.num_detection, snapshot.event_duration,
          snapshot.monitor_code);

      switch (snapshot.state) {
        case EventDetector::kEventStart: {
          if (annotated_frame) {
            // Update the highest level in context (need lock)
            SetMonitorCurrentHighestLevel(snapshot.monitor_code, level_idx);
            AlarmImage(snapshot.monitor_code,
                       snapshot.start_pts, snapshot.timestamp,
                       *annotated_frame, pict_writer,
                       snapshot.levels.second[level_idx], true);
          }
          break;
        }
        case EventDetector::kEventActive: {
          if (level_idx > snapshot.current_highest_level && annotated_frame) {
            // Update the highest level in context (need lock)
            SetMonitorCurrentHighestLevel(snapshot.monitor_code, level_idx);
          }
          break;
        }
        case EventDetector::kEventEnd: {
          // Reset the highest level in context (need lock)
          SetMonitorCurrentHighestLevel(snapshot.monitor_code, 0);
          // TODO: report event ended
          break;
        }
        default:
          break;
      }
    }

    if (combined_result.flushed_event) {
      if (auto& event_ptr = combined_result.flushed_event.value()) {
        scheduler->AddAlarm(std::make_unique<Alarm>(
            timer_.compute(event_ptr->start_timestamp), video_code_,
            std::move(event_ptr), out_width, out_height));
      }
    }

    if (auto out = std::atomic_load(&out_stream)) {
      if (combined_result.annotated_frame) {
        out->AddVideoFrame(std::move(combined_result.annotated_frame));
      }
    }

    av_frame_unref(frame_yuv420p.get());
  }

  std::vector<std::unique_ptr<Event>> tail_events;
  {
    std::lock_guard lock(event_manager_mutex_);
    if (event_manager_) {
      tail_events = event_manager_->FlushPending();
    }
  }
  for (auto& event_ptr : tail_events) {
    if (!event_ptr) {
      continue;
    }
    scheduler->AddAlarm(std::make_unique<Alarm>(
        timer_.compute(event_ptr->start_timestamp), video_code_,
        std::move(event_ptr), out_width, out_height));
  }
}


bool Worker::GrabFrame(AVFrame* frame) const {
  while (true) {
    auto ret = avcodec_receive_frame(in_stream->video_codec_ctx.get(), frame);
    if (ret == AVERROR(EAGAIN)) {
      auto pkg = in_stream->GetVideoPkt();
      if (!pkg) {
        return false;
      }
      if (ret =
          avcodec_send_packet(in_stream->video_codec_ctx.get(), pkg.get());
        ret < 0) {
        LOGE_FFMPEG("worker cannot send a packet:", ret);
        return false;
      }
      continue;
    }
    if (ret < 0) {
      LOGE_FFMPEG("worker cannot decode a packet:", ret);
      return false;
    }
    return true;
  }
}

bool Worker::ContainsMonitor(const std::string& code) const {
  std::shared_lock lock(monitors_mutex_);
  return std::ranges::any_of(monitors_,
                             [&code](const auto& ctx) { return ctx->monitor->code == code; });
}

const std::string& Worker::Code() const { return video_code_; }

void Worker::RemoveMonitor(const std::string& code) {
  bool remove_worker = false;
  {
    std::unique_lock lock(monitors_mutex_);
    const auto it = std::ranges::find_if(monitors_,
                                         [&code](const auto& ctx) {
                                           return ctx->monitor->code == code;
                                         });
    if (it == monitors_.end()) {
      LOGW("trying to remove unknown monitor code: {}", code);
      return;
    }

    if (monitors_.size() == 1) {
      remove_worker = true;
    } else {
      monitors_.erase(it);
    }
  }

  if (remove_worker) {
    Remove();
  } else {
    ClosePushThreadIfNeeded();
    RebuildEventManager();

    // Refresh annotator legend after a monitor removal
    EnsureAnnotatorReady();

    std::scoped_lock lock(annotator_mutex_);
    if (annotator_) {
      annotator_->update_legend(GetMonitorTasks());
    }
  }
}

void Worker::PrepareMonitorForStream(MonitorContext& ctx) const {
  if (!video_context_.valid()) {
    return;
  }
  auto& monitor = *ctx.monitor;
  monitor.roi_points.clear();
  if (!monitor.ParseRecognitionRegion(video_context_.width,
                                      video_context_.height)) {
    LOGE("ParseRecognitionRegion() error for monitor {}", monitor.code);
  }
  ctx.analyzer->SetROI();
  ctx.roi_entries.clear();
  if (!monitor.roi_points.empty()) {
    ctx.roi_entries.emplace_back(monitor.task, monitor.roi_points);
  }
}

void Worker::EnsureAllPipelinesReady(int out_width, int out_height) {
  if (!video_context_.valid() || video_context_.format == AV_PIX_FMT_NONE) {
    return;
  }

  EnsureAnnotatorReady();

  std::unique_lock lock(monitors_mutex_);
  for (auto& ctx_ptr : monitors_) {
    auto& ctx = *ctx_ptr;
    PrepareMonitorForStream(ctx);
    if (!ctx.analyzer) {
      try {
        ctx.analyzer = std::make_unique<Analyzer>(ctx.monitor.get(),
                                                  scheduler->GetConfig(),
                                                  ctx.monitor->conf_threshold);
      } catch (const std::exception& ex) {
        LOGE("Failed to create analyzer for monitor {}: {}", ctx.monitor->code,
             ex.what());
        continue;
      }
    }
    if (ctx.analyzer && !ctx.detector) {
      try {
        FormatConvertor convertor(video_context_.width, video_context_.height,
                                  video_context_.format, out_width, out_height);
        ctx.detector = std::make_unique<EventDetector>(
            ctx.event_cfg, *ctx.analyzer, std::move(convertor));
        ctx.current_highest_level = 0;
      } catch (const std::exception& ex) {
        LOGE("Failed to create event detector for monitor {}: {}",
             ctx.monitor->code, ex.what());
      }
    }
  }
}

void Worker::SetMonitorCurrentHighestLevel(std::string_view monitor_code, int level) {
  std::unique_lock lock(monitors_mutex_);
  for (auto& ctx : monitors_) {
    if (ctx && ctx->monitor && ctx->monitor->code == monitor_code) {
      ctx->current_highest_level = level;
      break;
    }
  }
}

void Worker::StartPushThreadIfNeeded() {
  std::lock_guard lock(push_stream_mutex_);
  if (!NeedPushStream() || std::atomic_load(&out_stream) || !State() || !video_context_.valid()) {
    return;
  }

  auto out = std::make_shared<AvPushStream>(this);
  if (!out->Connect()) {
    LOGE("push stream connect error for {}", push_stream_url_);
    return;
  }

  std::atomic_store(&out_stream, out);
  std::lock_guard tlock(threads_mutex_);
  threads_.push_back(std::make_unique<std::thread>([out]() { out->EncodeVideo(); }));
}

void Worker::CleanUpPushThread() {
  std::unique_ptr<std::thread> push_thread;
  {
    std::lock_guard lock(threads_mutex_);
    if (threads_.size() == 3) {
      push_thread = std::move(threads_.back());
      threads_.pop_back();
    }
  }
  if (push_thread && push_thread->joinable()) {
    push_thread->join();
  }
  std::atomic_store(&out_stream, std::shared_ptr<AvPushStream>{});
}

void Worker::ClosePushThreadIfNeeded() {
  std::lock_guard lock(push_stream_mutex_);
  if (!NeedPushStream()) {
    if (auto out = std::atomic_load(&out_stream)) {
      out->SignalShutdown();
    }
    g_th_pool.detach_task([this] {
      CleanUpPushThread();
    });
  }
}

void Worker::OnStreamOpened(VideoContext context) {
  video_context_ = context;
  {
    std::unique_lock lock(monitors_mutex_);
    for (auto& ctx : monitors_) {
      PrepareMonitorForStream(*ctx);
    }
  }
  // Reinitialize or update annotator with current monitor tasks
  StartPushThreadIfNeeded();
}
} // namespace avs_analyzer