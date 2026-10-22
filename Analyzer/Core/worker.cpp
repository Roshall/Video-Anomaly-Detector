#include "worker.h"

#include <cstdint>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Utils/common.h"
#include "Utils/log.h"
#include "Utils/request.h"
#include "admission.h"
#include "alarm.h"
#include "alarm_sound.h"
#include "analyzer.h"
#include "av_pull_stream.h"
#include "av_push_stream.h"
#include "config.h"
#include "combined_event_manager.h"
#include "format_convertor.h"
#include "frame.h"
#include "helper.h"
#include "jpeg_encoder.h"
#include "monitor.h"
#include "scheduler.h"
#include "streamer.h"

extern "C" {
#include <libswresample/swresample.h>
}

namespace avs_analyzer {

namespace {
// Frame rate used for recording; av_guess_frame_rate handles VFR and streams
// where the codec framerate is unset.
int GuessFps(AVFormatContext* fmt, AVStream* stream) {
  if (!fmt || !stream) {
    return 25;
  }
  const AVRational rate = av_guess_frame_rate(fmt, stream, nullptr);
  if (rate.num <= 0 || rate.den <= 0) {
    return 25;
  }
  return std::max(1, static_cast<int>(std::lround(
                         static_cast<double>(rate.num) / rate.den)));
}
}  // namespace

// Utility: collect unique task-label pairs from the current worker's monitors.
std::vector<std::pair<Task, std::u8string>> Worker::GetMonitorTasks() const {
  std::vector<std::pair<Task, std::u8string>> tasks;
  const auto snapshot = monitors_.Snapshot();
  tasks.reserve(snapshot.size());
  for (const auto& ctx : snapshot) {
    if (!ctx || !ctx->monitor) continue;
    const auto t = ctx->monitor->task;
    const auto it = std::find_if(tasks.begin(), tasks.end(),
                                 [t](const auto& entry) {
                                   return entry.first == t;
                                 });
    if (it == tasks.end()) {
      const auto label = ctx->monitor->behavior.empty()
                             ? toChinese(t)
                             : ctx->monitor->behavior;
      tasks.emplace_back(t, label);
    }
  }
  return tasks;
}

Worker::Worker(Scheduler* scheduler, std::unique_ptr<Monitor> monitor,
               const EventDetectorConfig& event_cfg, bool file)
  : scheduler(scheduler), file_{file} {
  metrics_ = MetricsRegistry::Instance().RegisterWorker(
      monitor->video_code.empty() ? monitor->code : monitor->video_code);
  AddMonitor(std::move(monitor), event_cfg);
}

Worker::~Worker() {
  LOGI("worker[videoCode={}, streamUrl={}] shutting down.", video_code_,
       stream_url_);
  state_.store(false, std::memory_order_release);
  if (in_stream) {
    in_stream->SignalShutdown();
  }
  auto out = std::atomic_load(&out_stream);
  if (out) {
    out->SignalShutdown();
  }

  // Properly deregister all monitors to stop detectors and avoid dangling callbacks
  for (const auto& ctx : monitors_.Snapshot()) {
    if (ctx && ctx->analyzer) {
      ctx->analyzer->Stop();
      scheduler->DeregisterAnalyzerCallback(*ctx->analyzer);
    }
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
  MetricsRegistry::Instance().UnregisterWorker(video_code_);
}

void Worker::AddMonitor(std::unique_ptr<Monitor> monitor,
                        const EventDetectorConfig& event_cfg) {
  std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>> monitors;
  monitors.emplace_back(std::move(monitor), event_cfg);
  AddMonitorsBatch(std::move(monitors));
}

void Worker::AddMonitorsBatch(std::vector<std::pair<std::unique_ptr<Monitor>, EventDetectorConfig>>&& monitors) {
  if (monitors.empty()) {
    return;
  }

  // Build the contexts first; the registry lock is only held for the insert.
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
        std::shared_ptr<Analyzer>(CreateAnalyzer(monitor.get(), scheduler->GetConfig()));
    ctx->analyzer->SetMetrics(metrics_);
    const size_t depth = file_ ? static_cast<size_t>(scheduler->GetConfig().offline_pipeline_depth) : 1;
    ctx->analyzer->SetAddTaskCallback(
        scheduler->RegisterAnalyzerCallback(*ctx->analyzer, depth));
    try {
      ctx->detector = std::make_shared<EventDetector>(event_cfg, *ctx->analyzer);
    } catch (const std::exception& ex) {
      LOGE("Failed to create event detector for monitor {}: {}",
           monitor->code, ex.what());
      // Continue without detector - analyzer can still work
    }

    if (video_code_.empty()) {
      video_code_ = monitor->video_code;
    }
    if (stream_url_.empty()) {
      stream_url_ = monitor->stream_url;
    } else if (monitor->stream_url != stream_url_) {
      LOGW("Worker {} ignores monitor {} custom stream_url {}; using {}",
          video_code_, monitor->code, monitor->stream_url,
          stream_url_);
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
    PrepareMonitorForStream(*ctx);
    contexts.push_back(std::move(ctx));
  }

  monitors_.AddBatch(std::move(contexts));

  // Rebuild only once after all monitors are added
  RebuildEventManager();
  StartPushThreadIfNeeded();

  // Rebuild the annotator with the updated legend before publishing it.
  EnsureAnnotatorReady(true);
}

size_t Worker::num_task() const {
  return monitors_.Size();
}

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

  in_stream = std::make_unique<AvPullStream>(this);
  if (!in_stream->Connect(file_)) {
    msg = "pull stream connect error";
    in_stream.reset();
    settle_start(false);
    return false;
  }

  // Create convertor before any worker threads start using it.
  // Video context is set during AvPullStream::Connect() via OnStreamOpened().
  VideoContext video_ctx_copy;
  {
    std::shared_lock lock(video_context_mutex_);
    video_ctx_copy = video_context_;
  }

  if (NeedPushStream()) {
    std::lock_guard lock(push_stream_mutex_);
    if (!std::atomic_load(&out_stream)) {
      auto out = std::make_shared<AvPushStream>(this);
      if (!out->Connect()) {
        msg = "push stream connect error";
        settle_start(false);
        return false;
      }
      std::atomic_store(&out_stream, std::move(out));
    }
  }

  auto out = std::atomic_load(&out_stream);
  auto initial_convertor = std::make_shared<ConvertorState>();
  initial_convertor->width = video_ctx_copy.width;
  initial_convertor->height = video_ctx_copy.height;
  initial_convertor->format = video_ctx_copy.format;
  try {
    initial_convertor->convertor = std::make_shared<FormatConvertor>(
        video_ctx_copy.width, video_ctx_copy.height, video_ctx_copy.format,
        out ? out->width() : video_ctx_copy.width,
        out ? out->height() : video_ctx_copy.height);
  } catch (const std::exception& ex) {
    msg = std::format("format convertor init error: {}", ex.what());
    settle_start(false);
    return false;
  }
  std::atomic_store(&convertor_state_,
                    std::shared_ptr<const ConvertorState>(initial_convertor));

  if (auto resources = in_stream->Current()) {
    timer_.init(resources->fmt.get(), resources->video_stream);
  }

  state_.store(true, std::memory_order_release);
  {
    std::lock_guard lock(threads_mutex_);
    threads_.push_back(
        std::make_unique<std::thread>([this]() { this->in_stream->Read(); }));
    threads_.push_back(
        std::make_unique<std::thread>([this]() { this->DetectEvent(); }));
  }
  if (out) {
    EnsureAllPipelinesReady();
    std::lock_guard lock(threads_mutex_);
    threads_.push_back(std::make_unique<std::thread>(
        [out]() { out->EncodeVideo(); }));
  }
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
  if (admin_stream_active_notified_.exchange(false, std::memory_order_acq_rel)) {
    NotifyAdminStreamEvent("end");
  }
  if (in_stream) {
    in_stream->SignalShutdown();
  }
  auto out = std::atomic_load(&out_stream);
  if (out) {
    out->SignalShutdown();
  }
  // Only evict ourselves: a replacement worker may already hold this video
  // code if a new add raced with this one being stopped.
  scheduler->RemoveWorker(video_code_, this);
}

void Worker::Stop() { state_.store(false, std::memory_order_release); }

void Worker::OfflineAnalyze() {
  const auto monitors = monitors_.Snapshot();
  if (monitors.empty()) {
    LOGW("offline analyze skipped: worker {} has no monitors", video_code_);
    return;
  }

  // Notify the admin service and stop the detectors on every exit path.
  auto finish = [&]() {
    for (const auto& rt : monitors) {
      if (!rt || !rt->monitor) continue;
      const std::string url =
          std::format("{}/api/offline/finished?controlCode={}",
                      scheduler->GetConfig().admin_host, rt->monitor->code);
      std::string res;
      Request::get(url.c_str(), res);
      LOGI("offline analyzer response ({}): {}", rt->monitor->code, res);

      if (rt->analyzer) {
        rt->analyzer->Stop();
        scheduler->DeregisterAnalyzerCallback(*rt->analyzer);
      }
    }
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
  std::atomic_store(&convertor_state_,
                    std::shared_ptr<const ConvertorState>(offline_convertor));

  auto in_stream_ = in.stream();
  timer_.init(in.format_context(), in_stream_, true);

  for (const auto& rt : monitors) {
    if (!rt->monitor) {
      continue;
    }
        if (!rt->monitor->ParseRecognitionRegion(width, height)) {
      LOGE("ParseRecognitionRegion() error for monitor {}", rt->monitor->code);
      
    }
    if (rt->monitor->task == Task::kBunkOff) {
      rt->analyzer->SetROIs(rt->monitor->roi_polygons);
    } else {
      rt->analyzer->SetROIs({rt->monitor->roi_points});
    }
    rt->roi_entries.clear();
    if (!rt->monitor->roi_points.empty()) {
      rt->roi_entries.emplace_back(rt->monitor->task, rt->monitor->roi_points);
    }
  }

  VideoContext overlay_ctx{.time_base = in_time_base,
                           .sample_aspect_ratio = AVRational{1, 1},
                           .format = static_cast<AVPixelFormat>(example->
                             format),
                           .width = width,
                           .height = height,
                           .video_index = -1};
  std::shared_ptr<DynamicAnnotator> annotator;
  try {
    auto candidate = std::make_shared<DynamicAnnotator>();
    const int ret = candidate->init(overlay_ctx);
    if (ret < 0) {
      LOGE("offline analyze: failed to init annotator: {}", ret);
    } else {
      std::vector<std::pair<Task, std::u8string>> tasks;
      tasks.reserve(monitors.size());
      for (const auto& rt : monitors) {
        if (!rt->monitor) continue;
        const auto t = rt->monitor->task;
        const auto it = std::find_if(tasks.begin(), tasks.end(),
                                     [t](const auto& entry) {
                                       return entry.first == t;
                                     });
        if (it == tasks.end()) {
          const auto label = rt->monitor->behavior.empty()
                                 ? toChinese(t)
                                 : rt->monitor->behavior;
          tasks.emplace_back(t, label);
        }
      }
      candidate->update_legend(tasks);
      annotator = std::move(candidate);
    }
  } catch (const std::exception& ex) {
    LOGE("offline analyze: failed to init annotator: {}", ex.what());
  }
  std::shared_ptr<CombinedEventManager> event_manager;
  if (annotator) {
    event_manager = std::make_shared<CombinedEventManager>();
    event_manager->SetAnnotator(annotator);
    event_manager->UpdateConfig(ComputeCombinedEventConfig(true));
    event_manager->UpdateMemoryBudget(EventBufferBudgetBytes());
    std::atomic_store(&event_manager_, event_manager);
  }

  auto alarm_settings = std::make_shared<const Alarm>(
      scheduler->GetConfig().upload_dir, scheduler->GetConfig().admin_host,
      width, height, 25, Alarm::OutputMode::kMp4Only, file_);

  auto enqueue_offline_alarm = [&](std::unique_ptr<Event> event,
                                   int64_t alarm_id) {
    if (!event) {
      return;
    }
    const auto start_ts = timer_.compute(event->start_timestamp);
    AlarmJob job;
    job.alarm = alarm_settings;
    job.id = alarm_id;
    job.video_code = video_code_;
    job.start_ts = start_ts;
    job.event = std::move(event);
    scheduler->AddAlarm(std::move(job));
  };

  auto pict_writer = std::make_shared<JPEGEncoder>(width, height);

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
      std::max(1, scheduler->GetConfig().offline_pipeline_depth));

  auto enqueue_next = [&]() -> bool {
    const AVFrame* raw = in.NextFrame();
    if (!raw) {
      return false;
    }
    AVFrame* holder = av_frame_alloc();
    if (!holder || av_frame_ref(holder, raw) < 0) {
      av_frame_free(&holder);
      return false;
    }
    const auto timestamp = holder->best_effort_timestamp - start_timestamp;
    holder->best_effort_timestamp = timestamp;
    holder->time_base = in_time_base;
    holder->pict_type = AV_PICTURE_TYPE_NONE;
    const auto timestamp_ms =
        av_rescale_q(timestamp, in_time_base, AVRational{1, 1000});
    AddTasks(holder, in_time_base);
    pending.push_back({AVFramePtr(holder), timestamp_ms, timestamp});
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

    AVFrame* frame = current.frame.get();
    const auto timestamp_ms = current.timestamp_ms;
    const auto timestamp = current.timestamp;

    std::vector<CombinedEventManager::DetectorFrameInfo> info_vec;
    info_vec.reserve(monitors.size());
    std::vector<EventDetector::State> states;
    for (const auto& rt : monitors) {
      if (!rt->monitor || !rt->detector) {
        continue;
      }

      const auto state = rt->detector->ProcessFrame(timestamp_ms, timestamp);
      if (!rt->detector->detections().empty()) {
        info_vec.emplace_back(
          rt->monitor->code,
          rt->monitor->task,
          state,
          rt->detector->detections(),
          rt->roi_entries,
          rt->detector->event_start_timestamp(),
          rt->detector->start_ts());
      states.push_back(state);
      }
    }
    if (event_manager) {
      auto result = event_manager->ProcessFrame(
          frame, timestamp_ms, timestamp, info_vec);
      auto combined_event_id = event_manager->CurrentEventId();
      std::shared_ptr<const Frame> annotated_frame = result.annotated_frame;
      size_t idx = 0;
      for (const auto& rt : monitors) {
        if (!rt->monitor || !rt->detector || rt->detector->detections().empty()) {
          continue;
        }
        auto level_idx = ComputeLevelIndex(
            rt->monitor->levels, rt->monitor->level_mode,
            rt->detector->num_detection(), rt->detector->event_duration(),
            rt->monitor->code);

        const auto state = states[idx++];
        switch (state) {
          case EventDetector::kEventStart:
            if (annotated_frame) {
              rt->current_highest_level.store(level_idx);
              AlarmImage(rt->monitor->code, rt->detector->start_ts(),
                        rt->detector->timestamp(), annotated_frame, pict_writer,
                        rt->monitor->levels.second[level_idx], combined_event_id);
            }
            break;
          case EventDetector::kEventActive:
            if (level_idx > rt->current_highest_level.load() && annotated_frame) {
              rt->current_highest_level.store(level_idx);
            }
            break;
          case EventDetector::kEventEnd:
            rt->current_highest_level.store(0);
            break;
          default:
            break;
        }
      }
      if (result.flushed_event) {
        if (auto& event = result.flushed_event.value()) {
          enqueue_offline_alarm(std::move(event), combined_event_id);
        }
      }
    }
  }

  // Flush any remaining pending events at EOF
  if (event_manager) {
    const auto tail_id = event_manager->CurrentEventId();
    auto tail_events = event_manager->FlushPending();
    for (auto& event : tail_events) {
      enqueue_offline_alarm(std::move(event), tail_id);
    }
  }

  finish();
}

void Worker::AlarmImage(const std::string& monitor_code, int64_t start_pts,
                        int64_t event_pts, std::shared_ptr<const Frame> frame,
                        std::shared_ptr<JPEGEncoder> pict_writer,
                        std::string_view level,
                        int64_t alarm_id) noexcept {
  if (!frame || !frame->data) {
    LOGW("AlarmImage received empty frame for monitor {}", monitor_code);
    return;
  }
  LOGI("检测到异常，发送报警图片到后台视频管理平台，摄像头： {}，监控点：{}，警报id： {}， 等级：{}",
       video_code_, monitor_code, alarm_id, level);
  const auto event_timestamp = timer_.compute(event_pts);
  const auto start_timestamp = timer_.compute(start_pts);

  // Encode before enqueueing the job so the pending alarm does not retain a
  // full annotated frame.
  std::vector<uint8_t> encoded;
  try {
    if (pict_writer) {
      encoded = pict_writer->EncodeToBuffer(frame->data.get());
    }
  } catch (const std::exception& e) {
    LOGE("AlarmImage: failed to encode picture: {}", e.what());
    return;
  }
  if (encoded.empty()) {
    LOGE("AlarmImage: failed to encode picture for monitor {}", monitor_code);
    return;
  }

  g_th_pool.detach_task([conf = scheduler->GetConfig(),
        code = monitor_code, start_timestamp, event_timestamp,
        level = std::string(level), alarm_id, video_code = video_code_,
        encoded = std::move(encoded), is_file = file_]() {
        auto img_path =
            std::format("{}/alarms/images/{}/{}", conf.upload_dir, code,
                        is_file ? std::to_string(getCurTimestamp()) : FrameWallClock::to_iso8601_utc(start_timestamp));
        std::filesystem::create_directories(std::filesystem::path(img_path));
        auto time_str = FrameWallClock::to_iso8601_utc(event_timestamp);
        auto img_path_str = std::format("{}/{}.jpg", img_path, time_str);
        std::ofstream out(img_path_str, std::ios::binary);
        if (!out.is_open()) {
          LOGE("failed to open alarm image {}", img_path_str);
          return;
        }
        out.write(reinterpret_cast<const char*>(encoded.data()),
                  static_cast<std::streamsize>(encoded.size()));
        if (!out) {
          LOGE("failed to write alarm image {}", img_path_str);
          return;
        }
        out.close();
        LOGI("Saved alarm image {} ({} bytes)", img_path_str, encoded.size());

        const std::string url =
            std::format("{}/api/postAddAlarm", conf.admin_host);
        nlohmann::json param {
        {"imagePath", img_path_str.substr(conf.upload_dir.length() + 1)},
        {"controlCode", code},
        {"videoCode", video_code},
        {"alarmId", alarm_id},
        {"createTime", is_file ? std::format("{:.3f}", std::chrono::duration<double>(event_timestamp.time_since_epoch()).count()) : FrameWallClock::to_iso8601_utc(event_timestamp)},
        {"level", level},
        {"file", is_file}
        };
        const std::string data = param.dump();
        std::string response;
        Request::post(url.data(), data.data(), response);
        LOGI("sent alarm image to {}", conf.admin_host);
      });
}

bool Worker::NeedPushStream() const {
  const auto snapshot = monitors_.Snapshot();
  return std::ranges::any_of(
      snapshot, [](const auto& ctx) {
        return ctx && ctx->monitor && ctx->monitor->push_stream;
      });
}

int Worker::PushQueueMaxFrames() const {
  if (!scheduler) {
    return 50;
  }
  return scheduler->GetConfig().push_queue_max_frames;
}

void Worker::RecordPushFrameDrop() const {
  if (metrics_) {
    metrics_->push_frames_dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

size_t Worker::EventBufferBudgetBytes() const {
  if (!scheduler) {
    return 256ull * 1024 * 1024;
  }
  return static_cast<size_t>(
             scheduler->GetConfig().event_buffer_budget_mb) *
         1024 * 1024;
}

CombinedEventConfig Worker::ComputeCombinedEventConfig() const {
  return ComputeCombinedEventConfig(false);
}

CombinedEventConfig Worker::ComputeCombinedEventConfig(bool offline_mode) const {
  CombinedEventConfig cfg{};
  for (const auto& ctx : monitors_.Snapshot()) {
    if (!ctx) {
      continue;
    }
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
  if (scheduler) {
    cfg.slice_ms =
        scheduler->GetConfig().hls_config.segment_seconds * 1000;
  }
  if (offline_mode && cfg.max_event_duration_ms > 0) {
    cfg.slice_ms = cfg.max_event_duration_ms;
  }
  return cfg;
}

void Worker::RebuildEventManager() {
  const auto cfg = ComputeCombinedEventConfig();

  auto manager = std::atomic_load(&event_manager_);
  if (!manager) {
    manager = std::make_shared<CombinedEventManager>();
    std::shared_ptr<DynamicAnnotator> annotator;
    {
      std::lock_guard lock(annotator_mutex_);
      annotator = annotator_;
    }
    manager->SetAnnotator(std::move(annotator));
    std::atomic_store(&event_manager_, manager);
  }
  manager->UpdateConfig(cfg);
  manager->UpdateMemoryBudget(EventBufferBudgetBytes());
}

void Worker::EnsureAnnotatorReady(bool force_rebuild) {
  // Copy video context under its lock first
  VideoContext video_ctx_copy;
  {
    std::shared_lock video_lock(video_context_mutex_);
    if (!video_context_.valid() || video_context_.format == AV_PIX_FMT_NONE) {
      return;
    }
    video_ctx_copy = video_context_;
  }

  if (!force_rebuild) {
    std::lock_guard lock(annotator_mutex_);
    if (annotator_) {
      return;
    }
  }

  // Gather monitor tasks before touching annotator state.
  const auto tasks = GetMonitorTasks();

  std::shared_ptr<DynamicAnnotator> candidate;
  try {
    candidate = std::make_shared<DynamicAnnotator>();

    DynamicAnnotator::LegendConfig legend_cfg;
    if (scheduler) {
      const auto& conf = scheduler->GetConfig();
      if (!conf.font_path.empty()) {
        legend_cfg.font_path = conf.font_path;
      }
      if (!conf.legend_position.empty()) {
        legend_cfg.SetLocation(conf.legend_position);
      }
      legend_cfg.prealloc_max_labels = conf.legend_prealloc_max_labels;
      legend_cfg.prealloc_max_label_len = conf.legend_prealloc_max_label_len;
    }

    if (const int ret = candidate->init(video_ctx_copy, legend_cfg); ret < 0) {
      LOGE("Failed to initialize worker annotator ({}x{}, fmt={}): {}",
          video_ctx_copy.width, video_ctx_copy.height,
          static_cast<int>(video_ctx_copy.format), ret);
      return;
    }
    candidate->update_legend(tasks);
  } catch (const std::exception& ex) {
    LOGE("Failed to initialize worker annotator: {}", ex.what());
    return;
  }

  // Publish the fully built annotator; the detect thread only ever sees
  // immutable published instances, so no long lock is needed on the hot path.
  std::lock_guard lock(annotator_mutex_);
  annotator_ = candidate;
  if (auto manager = std::atomic_load(&event_manager_)) {
    manager->SetAnnotator(std::move(candidate));
  }
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
  const auto state = std::atomic_load(&convertor_state_);
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
  idx = std::min(idx, static_cast<int>(levels.second.size()) - 1);
  return idx;
}

uint64_t Worker::NextId() {
  static std::atomic<uint64_t> global_id = 0;
  return global_id.fetch_add(1, std::memory_order_relaxed);
}

void Worker::DetectEvent() {
  if (!in_stream) {
    LOGW("decode video without active pull stream");
    return;
  }
  const auto initial = in_stream->Current();
  if (!initial) {
    LOGW("decode video without open stream resources");
    return;
  }

  int width = initial->codec->width;
  int height = initial->codec->height;
  uint64_t current_generation = initial->generation;

  AVFramePtr frame_yuv420p{av_frame_alloc()};
  if (!frame_yuv420p) {
    LOGE("av_frame_alloc failed");
    return;
  }

  auto out_cached = std::atomic_load(&out_stream);
  int out_width = out_cached ? out_cached->width() : width;
  int out_height = out_cached ? out_cached->height() : height;

  int fps = GuessFps(initial->fmt.get(), initial->video_stream);

  // Immutable output settings; alarm jobs only carry per-event values.
  auto alarm_settings = std::make_shared<const Alarm>(
      scheduler->GetConfig().upload_dir, scheduler->GetConfig().admin_host,
      out_width, out_height, fps,
      file_ ? Alarm::OutputMode::kMp4Only : Alarm::OutputMode::kAuto, file_);

  auto pict_writer = std::make_shared<JPEGEncoder>(width, height);

  // HLS recorder for the active combined event, owned by this thread.
  std::unique_ptr<HlsEventWriter> hls_writer;
  bool hls_playlist_reported = false;
  std::chrono::system_clock::time_point hls_start_ts{};

  auto start_hls_writer = [&]() {
    hls_writer.reset();
    hls_playlist_reported = false;
    hls_start_ts = {};
    if (file_ || scheduler->GetConfig().upload_dir.empty()) {
      return;
    }
    std::string start_time =
        FrameWallClock::to_iso8601_utc(std::chrono::system_clock::now());
    std::filesystem::path hls_dir =
        std::filesystem::path(scheduler->GetConfig().upload_dir) / "alarms" /
        "videos" / video_code_ / start_time;
    std::error_code ec;
    std::filesystem::create_directories(hls_dir, ec);
    if (ec) {
      LOGW("Failed to create HLS directory {}: {}", hls_dir.string(),
           ec.message());
      return;
    }
    auto writer = std::make_unique<HlsEventWriter>(
        width, height, fps, (hls_dir / "stream").string(),
        scheduler->GetConfig().hls_config);
    if (writer->valid()) {
      hls_writer = std::move(writer);
      LOGI("HLS recorder initialized for stream {}", video_code_);
    } else {
      LOGW("Failed to initialize HLS recorder for stream {}", video_code_);
    }
  };

  auto enqueue_alarm = [&](std::unique_ptr<Event> event, int64_t alarm_id,
                           std::chrono::system_clock::time_point start_ts) {
    AlarmJob job;
    job.alarm = alarm_settings;
    job.id = alarm_id;
    job.video_code = video_code_;
    job.start_ts = start_ts;
    if (event && hls_writer) {
      const bool more_segments = event->more_segments;
      hls_writer->AddFrames(*event);
      job.captured_by_hls = true;
      if (!hls_playlist_reported) {
        if (auto playlist = hls_writer->paths()) {
          job.hls_playlist = *playlist;
          hls_playlist_reported = true;
        }
      }
      if (hls_start_ts == std::chrono::system_clock::time_point{}) {
        hls_start_ts = start_ts;
      }
      if (!more_segments) {
        job.hls_paths = hls_writer->Close();
        hls_writer.reset();
        if (job.hls_paths) {
          job.start_ts = hls_start_ts;
        }
      }
    }
    job.event = std::move(event);
    scheduler->AddAlarm(std::move(job));
  };

  EnsureAnnotatorReady();
  RebuildEventManager();

  int64_t last_combined_start_pts = 0;
  int64_t last_combined_event_id = 0;
  int consecutive_errors = 0;
  bool stream_done = false;

  while (State() && !stream_done) {
    StreamResourcesPtr frame_resources;
    switch (GrabFrame(frame_yuv420p.get(), frame_resources)) {
      case GrabResult::kOk:
        consecutive_errors = 0;
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
    }

    const auto timestamp = frame_yuv420p->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
      // AddTasks could not resolve a timestamp for this frame.
      av_frame_unref(frame_yuv420p.get());
      continue;
    }
    frame_yuv420p->pict_type = AV_PICTURE_TYPE_NONE;
    const AVRational frame_time_base = frame_yuv420p->time_base;
    const auto timestamp_ms =
        av_rescale_q(timestamp, frame_time_base, AVRational{1, 1000});

    // A reconnect may change the decoded geometry; rebuild per-stream output
    // resources before processing this frame.
    if (frame_resources &&
        (frame_resources->codec->width != width ||
         frame_resources->codec->height != height)) {
      if (hls_writer) {
        AlarmJob job;
        job.alarm = alarm_settings;
        job.id = last_combined_event_id;
        job.video_code = video_code_;
        job.start_ts = hls_start_ts == std::chrono::system_clock::time_point{}
                           ? std::chrono::system_clock::now()
                           : hls_start_ts;
        job.captured_by_hls = true;
        job.hls_paths = hls_writer->Close();
        hls_writer.reset();
        if (job.hls_paths) {
          scheduler->AddAlarm(std::move(job));
        }
      }
      width = frame_resources->codec->width;
      height = frame_resources->codec->height;
      fps = GuessFps(frame_resources->fmt.get(), frame_resources->video_stream);
      pict_writer = std::make_shared<JPEGEncoder>(width, height);
      const auto out_now = std::atomic_load(&out_stream);
      out_width = out_now ? out_now->width() : width;
      out_height = out_now ? out_now->height() : height;
      alarm_settings = std::make_shared<const Alarm>(
          scheduler->GetConfig().upload_dir, scheduler->GetConfig().admin_host,
          out_width, out_height, fps,
          file_ ? Alarm::OutputMode::kMp4Only : Alarm::OutputMode::kAuto,
          file_);
      LOGI("worker {} stream geometry changed to {}x{}", video_code_, width,
           height);
    }

    // Update cached out_stream for this frame
    auto out_this_frame = std::atomic_load(&out_stream);
    if (out_this_frame != out_cached) {
      out_cached = out_this_frame;
      out_width = out_cached ? out_cached->width() : width;
      out_height = out_cached ? out_cached->height() : height;
    }

    std::vector<CombinedEventManager::DetectorFrameInfo> detector_infos;
    std::vector<MonitorEventSnapshot> event_snapshots;
    CombinedEventManager::Result combined_result;

    // Snapshot the registry and release its lock before any detector waits on
    // inference, so monitor add/remove never blocks on the detect loop.
    const auto monitor_snapshot = monitors_.Snapshot();
    detector_infos.reserve(monitor_snapshot.size());
    event_snapshots.reserve(monitor_snapshot.size());

    for (const auto& ctx : monitor_snapshot) {
      if (!ctx || !ctx->active.load(std::memory_order_acquire) ||
          !ctx->detector) {
        continue;
      }
      const auto state = ctx->detector->ProcessFrame(timestamp_ms, timestamp);
      if (!ctx->detector->detections().empty()) {
        detector_infos.emplace_back(
            ctx->monitor->code,
            ctx->monitor->task,
            state,
            ctx->detector->detections(),
            ctx->roi_entries,
            ctx->detector->event_start_timestamp(),
            ctx->detector->start_ts());
        event_snapshots.push_back(
            {ctx,
             ctx->monitor->code,
             state,
             ctx->detector->start_ts(),
             ctx->detector->timestamp(),
             ctx->detector->num_detection(),
             ctx->detector->event_duration(),
             ctx->monitor->levels,
             ctx->monitor->level_mode,
             ctx->current_highest_level.load(std::memory_order_relaxed),
             {}});
      }
    }

    int64_t combined_event_id = -1;
    if (auto event_manager = std::atomic_load(&event_manager_)) {
      combined_result = event_manager->ProcessFrame(
          frame_yuv420p.get(), timestamp_ms, timestamp, detector_infos);
      combined_event_id = event_manager->CurrentEventId();
      last_combined_event_id = combined_event_id;
    }

    // Notify admin for combined event edges (start / end).
    if (combined_result.transition == CombinedEventManager::Result::Transition::kBecameActive) {
      if (!admin_stream_active_notified_.load(std::memory_order_acquire)) {
        admin_stream_active_notified_.store(true, std::memory_order_release);
        NotifyAdminStreamEvent("start");
        last_combined_start_pts = timestamp;
        start_hls_writer();
        LOGI("combined event {} started on {}", combined_event_id, video_code_);
      }
    } else if (combined_result.transition == CombinedEventManager::Result::Transition::kBecameIdle) {
      if (admin_stream_active_notified_.load(std::memory_order_acquire)) {
        admin_stream_active_notified_.store(false, std::memory_order_release);
        NotifyAdminStreamEvent("end");
      }
    }

    std::shared_ptr<const Frame> annotated_frame = combined_result.annotated_frame;

    // Process alarms using snapshots
    for (auto& snapshot : event_snapshots) {
      auto level_idx = ComputeLevelIndex(
          snapshot.levels, snapshot.level_mode,
          snapshot.num_detection, snapshot.event_duration,
          snapshot.monitor_code);

      switch (snapshot.state) {
        case EventDetector::kEventStart: {
          if (level_idx < 0) {
            LOGT("monitor {} metric below first threshold {}", snapshot.monitor_code,
                snapshot.levels.first.front());
            level_idx = 0;
          }
          if (annotated_frame) {
            // Update the highest level in the monitor context
            snapshot.ctx->current_highest_level.store(level_idx);
            if (auto* alarm = scheduler->GetAlarmController()) {
              alarm->trigger();
              LOGD("Alarm sound triggered for monitor {}", snapshot.monitor_code);
            }
            AlarmImage(snapshot.monitor_code, snapshot.start_pts,
                       snapshot.timestamp, annotated_frame, pict_writer,
                       snapshot.levels.second[level_idx], combined_event_id);
          }
          break;
        }
        case EventDetector::kEventActive: {
          if (level_idx > snapshot.current_highest_level && annotated_frame) {
            snapshot.ctx->current_highest_level.store(level_idx);
          }
          break;
        }
        case EventDetector::kEventEnd: {
          snapshot.ctx->current_highest_level.store(0);
          // TODO: report event ended
          break;
        }
        default:
          break;
      }
    }

    if (combined_result.flushed_event) {
      if (auto& event_ptr = combined_result.flushed_event.value()) {
        auto start_ts = timer_.compute(event_ptr->start_timestamp);
        enqueue_alarm(std::move(event_ptr), combined_event_id, start_ts);
      }
    }

    // Route annotated frames to output stream. Load each time because a
    // reconnect may replace the pushed stream instance.
    if (combined_result.annotated_frame) {
      if (auto out_now = std::atomic_load(&out_stream)) {
        out_now->AddVideoFrame(combined_result.annotated_frame);
      }
    }

    av_frame_unref(frame_yuv420p.get());
  }

  std::vector<std::unique_ptr<Event>> tail_events;
  bool had_active = false;
  int64_t tail_id = 0;
  if (auto event_manager = std::atomic_load(&event_manager_)) {
    had_active = event_manager->HasActiveEvent();
    tail_id = event_manager->CurrentEventId();
    tail_events = event_manager->FlushPending();
    // If we were previously active, ensure admin is notified about stream end on flush.
    if (had_active &&
        admin_stream_active_notified_.load(std::memory_order_acquire)) {
      admin_stream_active_notified_.store(false, std::memory_order_release);
      NotifyAdminStreamEvent("end");
    }
  }
  if (had_active && tail_events.empty() &&
      canceled_all_monitors_.load(std::memory_order_acquire)) {
    const auto start_ts = last_combined_start_pts > 0
                              ? timer_.compute(last_combined_start_pts)
                              : std::chrono::system_clock::now();
    AlarmJob job;
    job.alarm = alarm_settings;
    job.id = tail_id;
    job.video_code = video_code_;
    job.start_ts =
        hls_start_ts == std::chrono::system_clock::time_point{}
            ? start_ts
            : hls_start_ts;
    if (hls_writer) {
      job.captured_by_hls = true;
      job.hls_paths = hls_writer->Close();
      hls_writer.reset();
      if (!job.hls_paths) {
        job.start_ts = start_ts;
      }
    }
    scheduler->AddAlarm(std::move(job));
  }
  for (auto& event_ptr : tail_events) {
    if (!event_ptr) {
      continue;
    }
    auto start_ts = timer_.compute(event_ptr->start_timestamp);
    enqueue_alarm(std::move(event_ptr), tail_id, start_ts);
  }
}


GrabResult Worker::GrabFrame(AVFrame* frame, StreamResourcesPtr& resources) {
  while (true) {
    auto current = in_stream->Current();
    if (!current) {
      return GrabResult::kFatal;
    }
    if (current->generation != decoded_generation_) {
      // New decoder generation: drop frames buffered from the old stream.
      avcodec_flush_buffers(current->codec.get());
      decoded_generation_ = current->generation;
      last_frame_ts_ms_.store(-1, std::memory_order_relaxed);
    }

    int ret = avcodec_receive_frame(current->codec.get(), frame);
    if (ret == AVERROR(EAGAIN)) {
      StreamPacket packet;
      if (!in_stream->GetVideoPkt(packet)) {
        if (!State()) {
          return GrabResult::kFatal;
        }
        if (in_stream->Eof()) {
          return GrabResult::kEof;
        }
        return GrabResult::kReconnecting;
      }
      if (packet.generation != current->generation) {
        // Packet belongs to a stream generation that has been replaced.
        continue;
      }
      if (ret = avcodec_send_packet(current->codec.get(), packet.pkt.get());
          ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
          return GrabResult::kAgain;
        }
        LOGE_FFMPEG("worker cannot send a packet:", ret);
        return GrabResult::kDecodeError;
      }
      continue;
    }
    if (ret == AVERROR_EOF) {
      return GrabResult::kEof;
    }
    if (ret < 0) {
      LOGE_FFMPEG("worker cannot decode a packet:", ret);
      return GrabResult::kDecodeError;
    }

    resources = current;
    frame->time_base = current->time_base;
    frame->pict_type = AV_PICTURE_TYPE_NONE;
    AddTasks(frame, current->time_base, current->codec->framerate);
    return GrabResult::kOk;
  }
}

bool Worker::ContainsMonitor(const std::string& code) const {
  return monitors_.Contains(code);
}

const std::string& Worker::Code() const { return video_code_; }

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
  // blocking Stop()/Deregister work runs outside so the detect loop is never
  // blocked by an in-flight inference.
  const auto removed = monitors_.RemoveBatch(codes);
  for (const auto& ctx : removed) {
    if (ctx && ctx->analyzer) {
      ctx->analyzer->Stop();
      scheduler->DeregisterAnalyzerCallback(*ctx->analyzer);
    }
  }
  if (removed.empty()) {
    return;
  }

  if (monitors_.Empty()) {
    canceled_all_monitors_.store(true, std::memory_order_release);
    Remove();
    return;
  }

  // Rebuild only once after all monitors are removed
  ClosePushThreadIfNeeded();
  RebuildEventManager();
  // Rebuild the annotator with the updated legend.
  EnsureAnnotatorReady(true);
}

void Worker::PrepareMonitorForStream(MonitorContext& ctx) const {
  auto& monitor = *ctx.monitor;
  {
    std::shared_lock video_lock(video_context_mutex_);
    if (!video_context_.valid() || video_context_.format == AV_PIX_FMT_NONE ||
        video_context_.width <= 0 || video_context_.height <= 0) {
      return;
    }
    if (!monitor.ParseRecognitionRegion(video_context_.width, video_context_.height)) {
      LOGE("ParseRecognitionRegion() error for monitor {}", monitor.code);
      return;
    }
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

void Worker::EnsureAllPipelinesReady() {
  EnsureAnnotatorReady();

  for (const auto& ctx_ptr : monitors_.Snapshot()) {
    if (!ctx_ptr) {
      continue;
    }
    auto& ctx = *ctx_ptr;
    PrepareMonitorForStream(ctx);
    if (!ctx.analyzer) {
      try {
        ctx.analyzer =
            std::shared_ptr<Analyzer>(CreateAnalyzer(ctx.monitor.get(), scheduler->GetConfig()));
        ctx.analyzer->SetMetrics(metrics_);
        const size_t depth = file_ ? static_cast<size_t>(scheduler->GetConfig().offline_pipeline_depth) : 1;
        ctx.analyzer->SetAddTaskCallback(
            scheduler->RegisterAnalyzerCallback(*ctx.analyzer, depth));
        
      } catch (const std::exception& ex) {
        LOGE("Failed to create analyzer for monitor {}: {}", ctx.monitor->code,
             ex.what());
        continue;
      }
    }
    if (ctx.analyzer && !ctx.detector) {
      try {
        ctx.detector = std::make_shared<EventDetector>(
            ctx.event_cfg, *ctx.analyzer);
        ctx.current_highest_level.store(0);
      } catch (const std::exception& ex) {
        LOGE("Failed to create event detector for monitor {}: {}",
             ctx.monitor->code, ex.what());
      }
    }
  }
}

void Worker::NotifyAdminStreamEvent(std::string_view state) {
  if (!scheduler) return;
  const auto& conf = scheduler->GetConfig();
  if (conf.admin_host.empty() || video_code_.empty()) {
    return;
  }
  const std::string url = std::format("{}/api/stream_event?state={}&streamCode={}",
                                      conf.admin_host, state, video_code_);
  // Fire-and-forget async request to avoid blocking the DetectEvent loop.
  g_th_pool.detach_task([url]() {
    std::string response;
    Request::get(url.c_str(), response);
    LOGI("sent stream event to {}, response: {}", url, response);
  });
}

void Worker::StartPushThreadIfNeeded() {
  if (!NeedPushStream() || !State()) {
    return;
  }
  {
      std::shared_lock video_lock(video_context_mutex_);
      if (!video_context_.valid()) {
        return;
      }
  }

  std::lock_guard lock(push_stream_mutex_);
  // Recheck with lock to ensure consistency
  if (std::atomic_load(&out_stream)) {
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

void Worker::CleanUpPushThread(bool detach) {
  std::unique_ptr<std::thread> push_thread;
  {
    std::lock_guard lock(threads_mutex_);
    if (threads_.size() == 3) {
      push_thread = std::move(threads_.back());
      threads_.pop_back();
    }
  }
  if (push_thread && push_thread->joinable()) {
    if (detach) {
      g_th_pool.detach_task([thread = std::move(push_thread)] {
        thread->join();
      });
    } else {
      push_thread->join();
    }
  }
  std::atomic_store(&out_stream, std::shared_ptr<AvPushStream>{});
}

void Worker::ClosePushThreadIfNeeded() {
  if (!NeedPushStream()) {
    std::lock_guard lock(push_stream_mutex_);
    if (auto out = std::atomic_load(&out_stream)) {
      out->SignalShutdown();
    }
    CleanUpPushThread(true);
  }
}

void Worker::OnStreamOpened(VideoContext context) {
  VideoContext previous;
  {
    std::unique_lock video_lock(video_context_mutex_);
    previous = video_context_;
    video_context_ = context;
  }
  // Update atomic variables for fast-path getters
  video_index_.store(context.video_index, std::memory_order_release);
  video_width_.store(context.width, std::memory_order_release);
  video_height_.store(context.height, std::memory_order_release);

  for (const auto& ctx : monitors_.Snapshot()) {
    if (ctx) {
      PrepareMonitorForStream(*ctx);
    }
  }

  const bool changed = !previous.valid() || previous.width != context.width ||
                       previous.height != context.height ||
                       previous.format != context.format ||
                       previous.video_index != context.video_index;
  // The initial open happens before start() publishes the first convertor.
  if (!changed || !State()) {
    StartPushThreadIfNeeded();
    return;
  }

  // A reconnect may change geometry: rebuild the convertor and output stream.
  try {
    auto state = std::make_shared<ConvertorState>();
    state->width = context.width;
    state->height = context.height;
    state->format = context.format;
    state->convertor = std::make_shared<FormatConvertor>(
        context.width, context.height, context.format, context.width,
        context.height);
    std::atomic_store(&convertor_state_,
                      std::shared_ptr<const ConvertorState>(state));
    LOGI("worker {} rebuilt convertor for {}x{} format={}", video_code_,
         context.width, context.height, static_cast<int>(context.format));
  } catch (const std::exception& ex) {
    LOGE("worker {} failed to rebuild convertor: {}", video_code_, ex.what());
  }

  if (NeedPushStream()) {
    if (auto out = std::atomic_load(&out_stream)) {
      out->SignalShutdown();
      CleanUpPushThread(true);
    }
    StartPushThreadIfNeeded();
  }
}
} // namespace avs_analyzer