#include "av_pull_stream.h"
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <utility>

#include "Utils/log.h"
#include "config.h"
#include "video_processing/helper.h"

extern "C" {
#include <libavutil/dict.h>
}

namespace avs_analyzer {

AvPullStream::AvPullStream(PullStreamHost* host) : host_(host) { LOGI(""); }

AvPullStream::~AvPullStream() { CloseConnect(); }

bool AvPullStream::OpenResources(bool file, StreamResourcesPtr& out,
                                 std::string& error) {
  std::lock_guard lock(open_mutex_);

  auto resources = std::make_shared<StreamResources>();
  const std::string stream_url(host_->StreamUrl());

  AVDictionary* fmt_options{};
  if (!file) {
    av_dict_set(&fmt_options, "rtsp_transport", "tcp", 0);
    av_dict_set(&fmt_options, "stimeout", "10000000", 0);
    av_dict_set(&fmt_options, "rw_timeout", "1000000", 0);
  }

  AVFormatContext* fmt = nullptr;
  int ret = avformat_open_input(&fmt, stream_url.data(), nullptr, &fmt_options);
  if (fmt_options) {
    av_dict_free(&fmt_options);
    fmt_options = nullptr;
  }
  if (ret != 0) {
    if (fmt) {
      avformat_close_input(&fmt);
    }
    error = fmt::format("avformat_open_input error: url={}", stream_url);
    LOGE_FFMPEG(error, ret);
    return false;
  }
  resources->fmt.reset(fmt);

  if ((ret = avformat_find_stream_info(resources->fmt.get(), nullptr)) < 0) {
    error = "avformat_find_stream_info error";
    LOGE_FFMPEG(error, ret);
    return false;
  }

  const int video_index = av_find_best_stream(
      resources->fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_index < 0) {
    error = fmt::format("av_find_best_stream video error videoIndex={}",
                        video_index);
    LOGE(error);
    return false;
  }
  AVStream* stream = resources->fmt->streams[video_index];
  const AVCodecParameters* codec_par = stream->codecpar;

  const AVCodec* video_codec = avcodec_find_decoder(codec_par->codec_id);
  if (!video_codec) {
    error = "avcodec_find_decoder error";
    LOGE(error);
    return false;
  }

  resources->codec.reset(avcodec_alloc_context3(video_codec));
  if (!resources->codec) {
    error = "avcodec_alloc_context3 error";
    LOGE(error);
    return false;
  }
  if ((ret = avcodec_parameters_to_context(resources->codec.get(), codec_par)) <
      0) {
    error = "avcodec_parameters_to_context error";
    LOGE_FFMPEG(error, ret);
    return false;
  }
  if ((ret = avcodec_open2(resources->codec.get(), video_codec, nullptr)) < 0) {
    error = "avcodec_open2 error";
    LOGE_FFMPEG(error, ret);
    return false;
  }

  resources->video_stream = stream;
  resources->video_index = video_index;
  resources->time_base = stream->time_base;
  resources->ctx =
      VideoContext{stream->time_base, resources->codec->sample_aspect_ratio,
                   resources->codec->pix_fmt, resources->codec->width,
                   resources->codec->height, video_index};
  resources->generation = ++generation_;

  if (!resources->ctx.valid()) {
    error = "invalid video context";
    LOGE("{}: width={} height={} index={}", error, resources->ctx.width,
         resources->ctx.height, video_index);
    return false;
  }

  out = std::move(resources);
  return true;
}

void AvPullStream::PublishResources(StreamResourcesPtr resources) {
  // Packets queued for the previous generation are useless once the new
  // decoder is live; drop them so the detect thread does not decode stale data.
  ClearPacketQueue();
  // Rate-limit state is per generation: log the first drop after a reconnect.
  dropped_packets_.store(0, std::memory_order_relaxed);
  std::atomic_store(&current_, std::move(resources));
}

bool AvPullStream::Connect(bool file) {
  file_ = file;
  StreamResourcesPtr resources;
  std::string error;
  if (!OpenResources(file, resources, error)) {
    return false;
  }
  PublishResources(resources);
  connect_count.fetch_add(1, std::memory_order_relaxed);
  host_->OnStreamOpened(resources->ctx);
  return true;
}

bool AvPullStream::SleepInterruptible(int ms) {
  constexpr int kSliceMs = 100;
  for (int slept = 0; slept < ms; slept += kSliceMs) {
    if (shutdown_.load(std::memory_order_acquire) || !host_->State()) {
      return false;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(std::min(kSliceMs, ms - slept)));
  }
  return !shutdown_.load(std::memory_order_acquire) && host_->State();
}

bool AvPullStream::Reconnect() {
  const Config& conf = host_->GetConfig();
  const int attempts = std::max(1, conf.stream_reconnect_max_attempts);
  int backoff_ms = std::max(100, conf.stream_reconnect_backoff_ms);
  const int backoff_max_ms = std::max(backoff_ms, conf.stream_reconnect_backoff_max_ms);

  for (int attempt = 1; attempt <= attempts; ++attempt) {
    if (shutdown_.load(std::memory_order_acquire) || !host_->State()) {
      return false;
    }
    connect_count.fetch_add(1, std::memory_order_relaxed);
    host_->RecordReconnect();

    StreamResourcesPtr resources;
    std::string error;
    if (OpenResources(file_, resources, error)) {
      PublishResources(resources);
      host_->OnStreamOpened(resources->ctx);
      LOGI("reconnect success: attempt={}/{} connectCount={}", attempt, attempts,
           connect_count.load(std::memory_order_relaxed));
      return true;
    }

    LOGW("reconnect attempt {}/{} failed: {}", attempt, attempts, error);
    if (attempt == attempts) {
      break;
    }
    if (!SleepInterruptible(backoff_ms)) {
      return false;
    }
    backoff_ms = std::min(backoff_ms * 2, backoff_max_ms);
  }

  LOGE("pull stream reconnect attempts exhausted (max={})", attempts);
  return false;
}

bool AvPullStream::GetVideoPkt(StreamPacket& packet) {
  pkt_cs_.acquire();
  return video_pkt_queue_.try_dequeue(packet);
}

void AvPullStream::SignalShutdown() {
  shutdown_.store(true, std::memory_order_release);
  // Wake the detect thread blocked on the packet queue.
  pkt_cs_.release();
}

void AvPullStream::ClearPacketQueue() {
  StreamPacket packet;
  while (video_pkt_queue_.try_dequeue(packet)) {
    // Consume the token that was released for this packet.
    pkt_cs_.try_acquire();
  }
}

void AvPullStream::Read() {
  AVPacketPtr pkt{av_packet_alloc()};
  if (!pkt) {
    LOGE("av_packet_alloc failed");
    host_->OnStreamError();
    return;
  }

  while (host_->State() && !shutdown_.load(std::memory_order_acquire)) {
    const StreamResourcesPtr resources = std::atomic_load(&current_);
    if (!resources) {
      LOGE("pull stream has no open resources");
      host_->OnStreamError();
      return;
    }

    const int ret = av_read_frame(resources->fmt.get(), pkt.get());
    if (ret >= 0) {
      if (pkt->stream_index == resources->video_index) {
        if (video_pkt_queue_.size_approx() > 300) {
          const uint64_t dropped =
              dropped_packets_.fetch_add(1, std::memory_order_relaxed) + 1;
          if (ShouldLogOverflow(dropped)) {
            LOGW("decode queue overflow: dropped {} packet(s), queue ~{}",
                 dropped, video_pkt_queue_.size_approx());
          }
          av_packet_unref(pkt.get());
          continue;
        }
        StreamPacket packet;
        packet.generation = resources->generation;
        packet.pkt = std::move(pkt);
        video_pkt_queue_.enqueue(std::move(packet));
        pkt_cs_.release();
        pkt.reset(av_packet_alloc());
        if (!pkt) {
          LOGE("av_packet_alloc failed");
          host_->OnStreamError();
          return;
        }
      } else {
        av_packet_unref(pkt.get());
      }
      continue;
    }

    if (ret == AVERROR_EOF) {
      LOGI("pull stream reached EOF, stopping read loop");
      eof_.store(true, std::memory_order_release);
      SignalShutdown();
      return;
    }

    LOGE_FFMPEG("av_read_frame in pull stream failed", ret);
    if (shutdown_.load(std::memory_order_acquire) || !host_->State()) {
      return;
    }

    if (!Reconnect()) {
      // Give the detect thread a chance to drain/flush and exit.
      SignalShutdown();
      host_->OnStreamError();
      return;
    }
  }
}

void AvPullStream::CloseConnect() {
  ClearPacketQueue();
  SignalShutdown();
}

}  // namespace avs_analyzer
