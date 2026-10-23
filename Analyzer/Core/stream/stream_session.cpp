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

}  // namespace avs_analyzer
