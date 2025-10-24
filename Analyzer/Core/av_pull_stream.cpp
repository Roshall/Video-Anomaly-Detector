#include "av_pull_stream.h"


#include "scheduler.h"
#include "Utils/log.h"
#include "worker.h"
#include "video_processing/helper.h"
#include "monitor.h"

extern "C" {
#include <libavutil/dict.h>
}


namespace avs_analyzer {

AvPullStream::AvPullStream(Worker* worker) : worker_(worker) { LOGI(""); }

AvPullStream::~AvPullStream() {
  CloseConnect();
}

bool AvPullStream::Connect(bool file) {
  std::string_view stream_url = worker_->monitor_->stream_url;

  AVDictionary* fmt_options{};
  if (!file) {
    av_dict_set(&fmt_options, "rtsp_transport", "tcp",
                0);  // 设置rtsp底层网络协议 tcp or udp
    av_dict_set(&fmt_options, "stimeout", "10000000",
                0);  // 设置rtsp连接超时（单位 us）1秒=1000000
    av_dict_set(&fmt_options, "rw_timeout", "1000000",
                0);  // 设置rtmp/http-flv连接超时（单位 us
  }

  auto fmt = avformat_alloc_context();
  int ret =
      avformat_open_input(&fmt, stream_url.data(), nullptr, &fmt_options);
  fmt_ctx.reset(fmt);
  if(fmt_options) {
    av_dict_free(&fmt_options);
    fmt_options = nullptr;
  }
  if (ret != 0) {
    LOGE_FFMPEG(std::format("avformat_open_input error: url={}", stream_url), ret);
    return false;
  }

  if (ret = avformat_find_stream_info(fmt_ctx.get(), nullptr); ret < 0) {
    LOGE_FFMPEG("avformat_find_stream_info error", ret);
    return false;
  }

  // video start
  auto video_index =
      av_find_best_stream(fmt_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  worker_->monitor_->video_index = video_index;
  stream = fmt_ctx->streams[video_index];
  if (video_index > -1) {
    AVCodecParameters* videoCodecPar = stream->codecpar;

    const AVCodec* video_codec = nullptr;
    video_codec = avcodec_find_decoder(videoCodecPar->codec_id);
    if (!video_codec) {
      LOGE("avcodec_find_decoder error");
      return false;
    }

    video_codec_ctx.reset(avcodec_alloc_context3(video_codec));
    if (ret = avcodec_parameters_to_context(video_codec_ctx.get(), videoCodecPar); ret < 0) {
      LOGE_FFMPEG("avcodec_parameters_to_context error", ret);
      return false;
    }
    if (ret = avcodec_open2(video_codec_ctx.get(), video_codec, nullptr); ret < 0) {
      LOGE_FFMPEG("avcodec_open2 error", ret);
      return false;
    }
    // mVideoCodecCtx->thread_count = 1;

    file = fmt_ctx->streams[video_index];
    // TODO: video resize
    worker_->monitor_->video_width = video_codec_ctx->width;
    worker_->monitor_->video_height = video_codec_ctx->height;

    if (!worker_->monitor_->ParseRecognitionRegion()) {
      LOGE("ParseRecognitionRegion() error");
      return false;
    }

  } else {
    LOGE("av_find_best_stream video error videoIndex=%d",
         worker_->monitor_->video_index);
    return false;
  }
  // video end;

  if (worker_->monitor_->video_index <= -1) {
    return false;
  }

  connect_count++;

  return true;
}

bool AvPullStream::Reconnect() {
  if (connect_count <= 100) {
    CloseConnect();
    if (Connect(false)) {
      return true;
    } else {
      return false;
    }
  }
  return false;
}

AVPacketPtr AvPullStream::GetVideoPkt() {
  pkt_cs_.acquire();
  AVPacketPtr pkg;
  video_pkt_queue_.try_dequeue(pkg);
  return pkg;
}
void AvPullStream::SignalShutdown() {
  //sent a message to wake up the thread and let it terminated itself.
  pkt_cs_.release();
}
void AvPullStream::ClearPacketQueue() {
  AVPacketPtr video_pkt;
  while (pkt_cs_.try_acquire() && video_pkt_queue_.try_dequeue(video_pkt)) {
  }
}
void AvPullStream::Read() {
  int continuity_error_count = 0;
  int ret;
  AVPacketPtr pkt{av_packet_alloc()};
  while (worker_->getState()) {
    if (ret = av_read_frame(fmt_ctx.get(), pkt.get()); ret >= 0) {
      continuity_error_count = 0;
      if (pkt->stream_index == worker_->monitor_->video_index) {
        if (video_pkt_queue_.size_approx() > 30 * 10) {
          LOGE("too many package to encode, throw one away.");
          continue;
        }
        video_pkt_queue_.enqueue(std::move(pkt));
        pkt_cs_.release();
        pkt.reset(av_packet_alloc());
      }
    } else {
      LOGE_FFMPEG("av_read_frame in pull stream failed", ret);
      continuity_error_count++;
      // TODO: timeout reconnect to be configurable
      if (continuity_error_count > 5) {  // 大于5秒重启拉流连接

        LOGE("av_read_frame error, continuity_error_count = %d (s)",
             continuity_error_count);

        if (Reconnect()) {
          continuity_error_count = 0;
          LOGI("reConnect success : mConnectCount=%d", connect_count);
        } else {
          LOGI("reConnect error : mConnectCount=%d", connect_count);
          worker_->remove();
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    }
  }
}
void AvPullStream::CloseConnect() {
  ClearPacketQueue();
  SignalShutdown();
//  fmt_ctx.reset();
}
float AvPullStream::GetFps() const {
  if (stream->avg_frame_rate.num && stream->avg_frame_rate.den) {
    return static_cast<float>(stream->avg_frame_rate.num) / stream->avg_frame_rate.den;
  } else {
    return 25.0f;
  }
}
}
// namespace avs_analyzer
