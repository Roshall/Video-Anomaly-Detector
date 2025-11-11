#include "worker.h"

#include <json/value.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Utils/common.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "alarm.h"
#include "analyzer.h"
#include "annotator.h"
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
template <typename Vec>
auto chain_vectors(Vec& a, Vec& b) {
  return std::array{std::views::all(a), std::views::all(b)} | std::views::join;
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
  if (out_stream) {
    out_stream->SignalShutdown();
  }
  for (auto& thread : threads_) {
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
  auto* ctx_raw = ctx.get();

  {
    std::unique_lock lock(monitors_mutex_);
    if (stream_url_.empty()) {
      stream_url_ = ctx_raw->monitor->stream_url;
    }
    if (ctx_raw->monitor->stream_url != stream_url_) {
      LOGW("Worker {} ignores monitor {} custom stream_url {}; using {}",
           video_code_, ctx_raw->monitor->code, ctx_raw->monitor->stream_url,
           stream_url_);
      ctx_raw->monitor->stream_url = stream_url_;
    }
    monitors_.push_back(std::move(ctx));
    if (video_context_.valid()) {
      PrepareMonitorForStream(*ctx_raw);
    }
  }
  ctx_raw->analyzer = std::make_unique<Analyzer>(
      ctx_raw->monitor.get(), scheduler->GetConfig(),
      ctx_raw->monitor->conf_threshold);
  if (out_stream) {
    try {
      ctx->detector = std::make_unique<EventDetector>(
          event_cfg, *ctx->analyzer,
          FormatConvertor{video_context_.width, video_context_.height,
                          video_context_.format, out_stream->width(),
                          out_stream->height()});
    } catch (const std::exception& ex) {
      LOGE("Failed to create event detector for monitor {}: {}",
           ctx->monitor->code, ex.what());
    }
  }
  ctx->current_highest_level = 0;
  // FIXME: rebuilding event manager will lose all cache frame
  //  here we can simply update event manage by providing the added monitor
  //  and it might need to be protected by the monitors mutex
  RebuildEventManager();
  StartPushThreadIfNeeded();
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
    out_stream = std::make_unique<AvPushStream>(this);
    if (!out_stream->Connect()) {
      msg = "push stream connect error";
      out_stream.reset();
      return false;
    }
  }

  timer_.init(in_stream->fmt_ctx.get(), in_stream->stream);

  state_.store(true);
  threads_.push_back(
      std::make_unique<std::thread>([this]() { this->in_stream->Read(); }));
  threads_.push_back(
      std::make_unique<std::thread>([this]() { this->DecodeVideo(); }));
  if (out_stream) {
    EnsureAllPipelinesReady(out_stream->width(), out_stream->height());
    threads_.push_back(std::make_unique<std::thread>(
        [this]() { this->out_stream->EncodeVideo(); }));
  }
  return true;
}

bool Worker::State() const { return state_.load(); }

void Worker::Remove() {
  state_.store(false);
  if (in_stream) {
    in_stream->SignalShutdown();
  }
  if (out_stream) {
    out_stream->SignalShutdown();
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
  std::unique_ptr<VideoOverlay> overlay;
  try {
    overlay = std::make_unique<VideoOverlay>(monitor.roi_points, overlay_ctx);
  } catch (const std::exception& ex) {
    LOGE("offline analyze: failed to init overlay for {}: {}", monitor.code,
         ex.what());
  }
  JPEGEncoder pict_writer(width, height);
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

    CombinedEventManager::DetectorFrameInfo info;
    info.monitor_code = monitor.code;
    info.overlay = overlay.get();
    info.state = state;
    info.detections = &checker.detections();
    info.event_start_timestamp_ms = checker.event_start_timestamp();
    info.event_start_pts = checker.start_ts();

    std::vector<CombinedEventManager::DetectorFrameInfo> info_vec;
    info_vec.emplace_back(std::move(info));

    auto combined = event_manager.ProcessFrame(frame, timestamp_ms, timestamp,
                                               info_vec);
    Frame* annotated_frame = combined.annotated_frame.get();

    switch (state) {
      case EventDetector::kEventStart:
        if (annotated_frame) {
          AlarmImage(monitor, checker.start_ts(), checker.timestamp(),
                     *annotated_frame, pict_writer, level, false);
        }
        break;
      case EventDetector::kEventEnd:
        if (annotated_frame) {
          AlarmImage(monitor, checker.start_ts(), checker.timestamp(),
                     *annotated_frame, pict_writer, level, false);
        }
        break;
      default:
        break;
    }
    for (auto flush_pending = event_manager.FlushPending();
      auto&& event : chain_vectors(combined.flushed_events, flush_pending)) {
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
  }

  const std::string url =
      std::format("{}/api/offline/finished?code={}",
                  scheduler->GetConfig().admin_host, monitor.code);
  std::string res;
  Request::get(url.c_str(), res);
  LOGI("offline analyzer response: {}", res);
}

void Worker::AlarmImage(const Monitor& monitor, int64_t start_pts,
                        int64_t event_pts, const Frame& frame,
                        JPEGEncoder& pict_writer, std::string_view level,
                        bool push) noexcept {
  if (!frame.data) {
    LOGW("AlarmImage skipped for monitor {}: missing frame data",
         monitor.code);
    return;
  }

  const auto event_timestamp = timer_.compute(event_pts);
  const auto start_timestamp = timer_.compute(start_pts);

  AVFramePtr frame_clone{av_frame_clone(frame.data.get())};
  if (!frame_clone) {
    LOGE("AlarmImage failed to clone frame for monitor {}", monitor.code);
    return;
  }
  frame_clone->best_effort_timestamp = frame.data->best_effort_timestamp;
  frame_clone->pts = frame.data->pts;
  frame_clone->time_base = frame.data->time_base;

  auto async_frame = std::make_unique<Frame>();
  async_frame->timestamp = frame.timestamp;
  async_frame->object_count = frame.object_count;
  async_frame->data = std::move(frame_clone);

  g_th_pool.detach_task([&pict_writer, &conf = scheduler->GetConfig(),
        code = monitor.code, start_timestamp, event_timestamp,
        level = std::string(level), push,
        frame = std::move(async_frame)]() mutable {
        auto img_path =
            std::format("{}/alarm/{}/{}", conf.upload_dir, code,
                        FrameWallClock::to_iso8601_utc(start_timestamp));
        std::filesystem::create_directories(std::filesystem::path(img_path));
        auto time_str = FrameWallClock::to_iso8601_utc(event_timestamp);
        auto img_path_str = std::format("{}/{}.jpg", img_path, time_str);
        if (const int ret = pict_writer.saveFrame(
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
  event_manager_ = std::make_unique<CombinedEventManager>();
  event_manager_->UpdateConfig(ComputeCombinedEventConfig());
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

  int out_width = out_stream ? out_stream->width() : width;
  int out_height = out_stream ? out_stream->height() : height;

  JPEGEncoder pict_writer(width, height);

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

    out_width = out_stream ? out_stream->width() : width;
    out_height = out_stream ? out_stream->height() : height;

    std::vector<MonitorContext*> active_contexts;
    {
      std::shared_lock lock(monitors_mutex_);
      active_contexts.reserve(monitors_.size());
      for (auto& ctx : monitors_) {
        active_contexts.push_back(ctx.get());
      }
    }

    std::vector<CombinedEventManager::DetectorFrameInfo> detector_infos;
    detector_infos.reserve(active_contexts.size());
    std::vector<std::pair<MonitorContext*, EventDetector::State>>
        detector_states;
    detector_states.reserve(active_contexts.size());

    // FIXME: data race with RemoveMonitor function
    for (auto* ctx : active_contexts) {
      if (!ctx || !ctx->detector) {
        continue;
      }
      const auto state =
          ctx->detector->ProcessFrame({timestamp_ms, frame_yuv420p.get()});
      detector_states.emplace_back(ctx, state);

      CombinedEventManager::DetectorFrameInfo info;
      // TODO: use monitor.id instead of code
      info.monitor_code = ctx->monitor->code;
      info.overlay = ctx->overlay.get();
      info.state = state;
      info.detections = &ctx->detector->detections();
      info.event_start_timestamp_ms = ctx->detector->event_start_timestamp();
      info.event_start_pts = ctx->detector->start_ts();
      detector_infos.emplace_back(info);
    }

    CombinedEventManager::Result combined_result;
    if (event_manager_) {
      combined_result = event_manager_->ProcessFrame(
          frame_yuv420p.get(), timestamp_ms, timestamp, detector_infos);
    }

    Frame* annotated_frame = combined_result.annotated_frame.get();

    for (auto& [ctx, state] : detector_states) {
      if (!ctx || !ctx->detector) {
        continue;
      }
      // TODO: we check its level only when the state is start/active
      const auto& monitor = *ctx->monitor;
      auto compute_level_index = [&]() -> std::optional<int> {
        if (monitor.levels.first.empty() || monitor.levels.second.empty()) {
          return std::nullopt; // FIXME: this is a impossible state
        }
        int64_t metric = monitor.level_mode == LevelMode::kNumPeople
                           ? ctx->detector->num_detection()
                           : ctx->detector->event_duration();
        int idx = 0;
        const int total = static_cast<int>(monitor.levels.first.size());
        while (idx < total && monitor.levels.first[idx] <= metric) {
          ++idx;
        }
        --idx;
        if (idx < 0) {
          LOGW("monitor {} metric {} below first threshold {}", monitor.code,
               metric, monitor.levels.first.front());
          idx = 0;
        }
        idx = std::min(idx, static_cast<int>(monitor.levels.second.size()) - 1);
        return idx;
      };

      switch (state) {
        case EventDetector::kEventStart: {
          if (auto level_idx = compute_level_index()) {
            ctx->current_highest_level = *level_idx;
            if (annotated_frame) {
              AlarmImage(monitor, ctx->detector->start_ts(),
                         ctx->detector->timestamp(), *annotated_frame,
                         pict_writer, monitor.levels.second[*level_idx], true);
            }
          }
          break;
        }
        case EventDetector::kEventActive: {
          if (auto level_idx = compute_level_index()) {
            if (*level_idx > ctx->current_highest_level && annotated_frame) {
              AlarmImage(monitor, ctx->detector->start_ts(),
                         ctx->detector->timestamp(), *annotated_frame,
                         pict_writer, monitor.levels.second[*level_idx], true);
              ctx->current_highest_level = *level_idx;
            }
          }
          break;
        }
        case EventDetector::kEventEnd: {
          if (auto level_idx = compute_level_index();
            level_idx && annotated_frame) {
            AlarmImage(monitor, ctx->detector->start_ts(),
                       ctx->detector->timestamp(), *annotated_frame,
                       pict_writer, monitor.levels.second[*level_idx], true);
          }
          ctx->current_highest_level = 0;
          break;
        }
        default:
          break;
      }
    }

    const std::string control_code =
        !video_code_.empty()
          ? video_code_
          : (!active_contexts.empty()
               ? active_contexts.front()->monitor->code
               : std::string{});

    if (!control_code.empty()) {
      for (auto& event_ptr : combined_result.flushed_events) {
        if (!event_ptr) {
          continue;
        }
        scheduler->AddAlarm(std::make_unique<Alarm>(
            timer_.compute(event_ptr->start_timestamp), control_code,
            std::move(event_ptr), out_width, out_height));
      }
    }

    if (out_stream && combined_result.annotated_frame) {
      out_stream->AddVideoFrame(std::move(combined_result.annotated_frame));
    }

    av_frame_unref(frame_yuv420p.get());
  }

  if (event_manager_) {
    auto tail_events = event_manager_->FlushPending();
    std::string control_code = video_code_;
    if (control_code.empty()) {
      std::shared_lock lock(monitors_mutex_);
      if (!monitors_.empty()) {
        control_code = monitors_.front()->monitor->code;
      }
    }
    if (!control_code.empty()) {
      for (auto& event_ptr : tail_events) {
        if (!event_ptr) continue;
        scheduler->AddAlarm(std::make_unique<Alarm>(
            timer_.compute(event_ptr->start_timestamp), control_code,
            std::move(event_ptr), out_width, out_height));
      }
    }
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
    // FIXME: rebuilding event manager will lose all cache frame
    //  here we can simply update event manage by providing the to-be-removed monitor
    //  and it might need to be protected by the monitors mutex
    RebuildEventManager();
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
  try {
    ctx.overlay = std::make_unique<VideoOverlay>(monitor.roi_points,
                                                 video_context_);
  } catch (const std::exception& ex) {
    LOGE("Failed to initialize overlay for monitor {}: {}", monitor.code,
         ex.what());
    ctx.overlay.reset();
  }
}

void Worker::EnsureAllPipelinesReady(int out_width, int out_height) const {
  if (!video_context_.valid() || video_context_.format == AV_PIX_FMT_NONE) {
    return;
  }
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

void Worker::StartPushThreadIfNeeded() {
  if (!NeedPushStream() || out_stream || !State() || !video_context_.valid()) {
    return;
  }

  out_stream = std::make_unique<AvPushStream>(this);
  if (!out_stream->Connect()) {
    LOGE("push stream connect error for {}", push_stream_url_);
    out_stream.reset();
    return;
  }
  threads_.push_back(std::make_unique<std::thread>(
      [this]() { this->out_stream->EncodeVideo(); }));
}

void Worker::CleanUpPushThread() {
  std::lock_guard lock(push_stream_mutex_);
  if (threads_.size() == 3) {
    const auto push_thread = std::move(threads_.back());
    threads_.pop_back();
    if (push_thread->joinable()) {
      push_thread->join();
    }
    out_stream.reset();
  }
}

void Worker::ClosePushThreadIfNeeded() {
  if (!NeedPushStream() && out_stream) {
    out_stream->SignalShutdown();
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

  StartPushThreadIfNeeded();
}
} // namespace avs_analyzer