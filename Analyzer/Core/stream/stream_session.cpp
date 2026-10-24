#include "stream/stream_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>

#include "Utils/log.h"
#include "config.h"
#include "scheduler.h"

namespace avs_analyzer {
namespace {

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

StreamSession::StreamSession(Options options)
    : options_(std::move(options)),
      scheduler_(options_.scheduler),
      stream_url_(std::move(options_.stream_url)),
      push_stream_url_(std::move(options_.push_stream_url)),
      file_(options_.file),
      state_(options_.state),
      metrics_(std::move(options_.metrics)),
      on_stream_error_(std::move(options_.on_stream_error)),
      on_stream_opened_(std::move(options_.on_stream_opened)) {}

StreamSession::~StreamSession() {
  Stop();
  if (read_thread_ && read_thread_->joinable()) {
    read_thread_->join();
  }
  if (push_thread_ && push_thread_->joinable()) {
    push_thread_->join();
  }
  out_stream_.reset();
}

bool StreamSession::Open(std::string& msg) {
  in_stream_ = std::make_unique<AvPullStream>(this);
  if (!in_stream_->Connect(file_)) {
    msg = "pull stream connect error";
    in_stream_.reset();
    return false;
  }

  const VideoContext context = VideoContextSnapshot();
  if (push_requested_.load(std::memory_order_acquire) && !out_stream_) {
    std::lock_guard lock(push_mutex_);
    if (!out_stream_) {
      auto out = std::make_shared<AvPushStream>(this);
      if (!out->Connect()) {
        msg = "push stream connect error";
        return false;
      }
      std::atomic_store(&out_stream_, std::move(out));
    }
  }

  const auto out = std::atomic_load(&out_stream_);
  RebuildConvertor(context, out ? out->width() : context.width,
                   out ? out->height() : context.height);
  return true;
}

void StreamSession::StartThreads() {
  if (!in_stream_) {
    return;
  }
  read_thread_ = std::make_unique<std::thread>(
      [this]() { in_stream_->Read(); });

  if (auto out = std::atomic_load(&out_stream_)) {
    std::lock_guard lock(push_mutex_);
    if (!push_thread_) {
      push_thread_ = std::make_unique<std::thread>(
          [out]() { out->EncodeVideo(); });
    }
  }
}

void StreamSession::Stop() {
  if (in_stream_) {
    in_stream_->SignalShutdown();
  }
  if (auto out = std::atomic_load(&out_stream_)) {
    out->SignalShutdown();
  }
}

void StreamSession::SetPushRequested(bool requested, std::string push_url) {
  push_stream_url_ = std::move(push_url);
  push_requested_.store(requested, std::memory_order_release);
  if (requested) {
    EnsurePush();
  } else {
    ClosePush(true);
  }
}

void StreamSession::EnsurePush() {
  if (!NeedPushStream() || !State()) {
    return;
  }
  {
    std::unique_lock lock(video_context_mutex_);
    if (!video_context_.valid()) {
      return;
    }
  }

  std::lock_guard lock(push_mutex_);
  if (std::atomic_load(&out_stream_)) {
    return;
  }

  auto out = std::make_shared<AvPushStream>(this);
  if (!out->Connect()) {
    LOGE("push stream connect error for {}", push_stream_url_);
    return;
  }

  std::atomic_store(&out_stream_, std::move(out));
  push_thread_ = std::make_unique<std::thread>(
      [out]() { out->EncodeVideo(); });
}

void StreamSession::ClosePush(bool detach) {
  std::unique_ptr<std::thread> thread;
  {
    std::lock_guard lock(push_mutex_);
    if (auto out = std::atomic_load(&out_stream_)) {
      out->SignalShutdown();
    }
    thread = std::move(push_thread_);
  }
  std::atomic_store(&out_stream_, std::shared_ptr<AvPushStream>{});
  JoinPushThread(std::move(thread), detach);
}

void StreamSession::JoinPushThread(std::unique_ptr<std::thread> thread,
                                   bool detach) {
  if (!thread || !thread->joinable()) {
    return;
  }
  if (detach) {
    g_control_pool.detach_task(
        [thread = std::move(thread)]() {
          if (thread && thread->joinable()) {
            thread->join();
          }
        });
  } else {
    thread->join();
  }
}

void StreamSession::OnPushStreamError() { ClosePush(true); }

int StreamSession::Fps() const {
  const auto resources = Current();
  if (!resources) {
    return 25;
  }
  return GuessFps(resources->fmt.get(), resources->video_stream);
}

VideoContext StreamSession::VideoContextSnapshot() const {
  std::shared_lock lock(video_context_mutex_);
  return video_context_;
}

const Config& StreamSession::GetConfig() const {
  return scheduler_->GetConfig();
}

int StreamSession::PushQueueMaxFrames() const {
  return scheduler_ ? scheduler_->GetConfig().push_queue_max_frames : 50;
}

void StreamSession::RecordPushFrameDrop() const {
  if (metrics_) {
    metrics_->push_frames_dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void StreamSession::RecordReconnect() {
  if (metrics_) {
    metrics_->reconnects.fetch_add(1, std::memory_order_relaxed);
  }
}

void StreamSession::OnStreamError() {
  if (on_stream_error_) {
    on_stream_error_();
  }
}

void StreamSession::RebuildConvertor(const VideoContext& context,
                                     int out_width, int out_height) {
  try {
    auto state = std::make_shared<ConvertorState>();
    state->width = context.width;
    state->height = context.height;
    state->format = context.format;
    state->convertor = std::make_shared<FormatConvertor>(
        context.width, context.height, context.format, out_width, out_height);
    std::atomic_store(&convertor_state_,
                      std::shared_ptr<const ConvertorState>(state));
  } catch (const std::exception& ex) {
    LOGE("failed to initialise stream convertor: {}", ex.what());
  }
}

void StreamSession::OnStreamOpened(const VideoContext& context) {
  VideoContext previous;
  {
    std::unique_lock lock(video_context_mutex_);
    previous = video_context_;
    video_context_ = context;
  }
  video_index_.store(context.video_index, std::memory_order_release);
  video_width_.store(context.width, std::memory_order_release);
  video_height_.store(context.height, std::memory_order_release);

  // Let the Worker prepare monitors/annotators for the new geometry.
  if (on_stream_opened_) {
    on_stream_opened_(context);
  }

  const bool changed = !previous.valid() || previous.width != context.width ||
                       previous.height != context.height ||
                       previous.format != context.format ||
                       previous.video_index != context.video_index;
  if (!changed) {
    EnsurePush();
    return;
  }
  if (!State()) {
    return;
  }

  if (const auto out = std::atomic_load(&out_stream_)) {
    out->SignalShutdown();
  }
  ClosePush(true);
  RebuildConvertor(context, context.width, context.height);
  LOGI("stream {} rebuilt convertor for {}x{} format={}", options_.video_code,
       context.width, context.height, static_cast<int>(context.format));
  EnsurePush();
}

GrabResult StreamSession::GrabFrame(AVFrame* frame,
                                    StreamResourcesPtr& resources) {
  while (true) {
    auto current = in_stream_->Current();
    if (!current) {
      return GrabResult::kFatal;
    }
    if (current->generation != decoded_generation_) {
      // New decoder generation: drop frames buffered from the old stream.
      avcodec_flush_buffers(current->codec.get());
      decoded_generation_ = current->generation;
    }

    int ret = avcodec_receive_frame(current->codec.get(), frame);
    if (ret == AVERROR(EAGAIN)) {
      StreamPacket packet;
      if (!in_stream_->GetVideoPkt(packet)) {
        if (!State()) {
          return GrabResult::kFatal;
        }
        if (in_stream_->Eof()) {
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
    return GrabResult::kOk;
  }
}

void StreamSession::PushFrame(std::shared_ptr<const Frame> frame) {
  if (auto out = std::atomic_load(&out_stream_)) {
    out->AddVideoFrame(std::move(frame));
  }
}

void StreamSession::RunDecodeLoop(const DecodeLoopHandlers& handlers) {
  const auto initial = Current();
  if (!initial) {
    LOGW("decode video without open stream resources");
    return;
  }

  int width = initial->codec->width;
  int height = initial->codec->height;
  uint64_t current_generation = initial->generation;
  int fps = Fps();
  int out_width = PushWidth();
  int out_height = PushHeight();

  AVFramePtr frame{av_frame_alloc()};
  if (!frame) {
    LOGE("av_frame_alloc failed");
    return;
  }

  int consecutive_errors = 0;
  bool done = false;

  while (State() && !done) {
    StreamResourcesPtr frame_resources;
    switch (GrabFrame(frame.get(), frame_resources)) {
      case GrabResult::kOk: {
        consecutive_errors = 0;
        if (metrics_) {
          metrics_->frames_decoded.fetch_add(1, std::memory_order_relaxed);
        }

        if (frame_resources &&
            frame_resources->generation != current_generation) {
          current_generation = frame_resources->generation;
          if (options_.timer) {
            options_.timer->init(frame_resources->fmt.get(),
                                 frame_resources->video_stream);
          }
          // Reset before resolving: a stream without PTS restarts its
          // synthesised timeline on the new generation, matching sources
          // whose PTS itself restarts after a reconnect.
          last_frame_ts_ms_.store(-1, std::memory_order_relaxed);
          if (handlers.on_new_generation) {
            handlers.on_new_generation();
          }
        }

        int64_t timestamp = 0;
        int64_t timestamp_ms = 0;
        const AVRational fps_hint =
            frame_resources ? frame_resources->codec->framerate
                            : AVRational{0, 1};
        if (!ResolveTimestamps(frame.get(), frame->time_base, fps_hint,
                               timestamp, timestamp_ms)) {
          if (metrics_) {
            metrics_->frames_skipped.fetch_add(1, std::memory_order_relaxed);
          }
          av_frame_unref(frame.get());
          continue;
        }

        // A reconnect may change the decoded geometry; rebuild per-stream
        // output resources before processing this frame.
        if (frame_resources &&
            (frame_resources->codec->width != width ||
             frame_resources->codec->height != height)) {
          width = frame_resources->codec->width;
          height = frame_resources->codec->height;
          fps = Fps();
          out_width = PushWidth();
          out_height = PushHeight();
          if (handlers.on_geometry_changed) {
            handlers.on_geometry_changed(width, height, fps, out_width,
                                         out_height);
          }
        }

        DecodedFrame decoded{frame.get(), timestamp, timestamp_ms};
        const bool keep_going =
            handlers.on_frame ? handlers.on_frame(decoded) : true;
        av_frame_unref(frame.get());
        if (!keep_going) {
          done = true;
        }
        break;
      }
      case GrabResult::kAgain:
      case GrabResult::kDecodeError: {
        if (++consecutive_errors >= 50) {
          LOGE("worker {} exceeded decode error budget, removing worker",
               options_.video_code);
          if (handlers.on_error_budget_exhausted) {
            handlers.on_error_budget_exhausted();
          }
          done = true;
          break;
        }
        const int delay_ms = std::min(500, 10 * consecutive_errors);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        break;
      }
      case GrabResult::kReconnecting:
        // A reconnect is in progress; wait briefly instead of spinning.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        break;
      case GrabResult::kEof:
        LOGI("worker {} reached end of stream", options_.video_code);
        done = true;
        break;
      case GrabResult::kFatal:
        done = true;
        break;
    }
  }
}

bool StreamSession::ResolveTimestamps(AVFrame* frame, AVRational time_base,
                                      AVRational fps_hint,
                                      int64_t& timestamp,
                                      int64_t& timestamp_ms) {
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
           options_.video_code);
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

}  // namespace avs_analyzer
