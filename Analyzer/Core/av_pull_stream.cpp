#include "av_pull_stream.h"


#include "scheduler.h"
#include "Utils/log.h"
#include "worker.h"
#include "video_processing/helper.h"
#include "control.h"

extern "C" {
#include <libavutil/dict.h>
}


namespace avs_analyzer {

AvPullStream::AvPullStream(Worker* worker) : worker_(worker) { LOGI(""); }

AvPullStream::~AvPullStream() { SignalShutdown();}

bool AvPullStream::Connect() {
  std::string_view stream_url = worker_->monitor->stream_url;


  AVDictionary* fmt_options = nullptr;
  av_dict_set(&fmt_options, "rtsp_transport", "tcp",
              0);  // 设置rtsp底层网络协议 tcp or udp
  av_dict_set(&fmt_options, "stimeout", "10000000",
              0);  // 设置rtsp连接超时（单位 us）1秒=1000000
  av_dict_set(&fmt_options, "rw_timeout", "1000000",
              0);  // 设置rtmp/http-flv连接超时（单位 us）
  // av_dict_set(&fmt_options, "timeout", "1000000", 0);//设置udp/http超时（单位
  // us）
  auto fmt = avformat_alloc_context();
  int ret =
      avformat_open_input(&fmt, stream_url.data(), nullptr, &fmt_options);
  fmt_ctx.reset(fmt);

  if (ret != 0) {
    LOGE("avformat_open_input error: url=%s ", stream_url.data());
    return false;
  }

  if (avformat_find_stream_info(fmt_ctx.get(), nullptr) < 0) {
    LOGE("avformat_find_stream_info error");
    return false;
  }

  // video start
  auto video_index =
      av_find_best_stream(fmt_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  worker_->monitor->video_index = video_index;
  if (video_index > -1) {
    AVCodecParameters* videoCodecPar = fmt_ctx->streams[video_index]->codecpar;

    const AVCodec* video_codec = nullptr;
    video_codec = avcodec_find_decoder(videoCodecPar->codec_id);
    if (!video_codec) {
      LOGE("avcodec_find_decoder error");
      return false;
    }

    video_codec_ctx.reset(avcodec_alloc_context3(video_codec));
    if (avcodec_parameters_to_context(video_codec_ctx.get(), videoCodecPar) != 0) {
      LOGE("avcodec_parameters_to_context error");
      return false;
    }
    if (avcodec_open2(video_codec_ctx.get(), video_codec, nullptr) < 0) {
      LOGE("avcodec_open2 error");
      return false;
    }
    // mVideoCodecCtx->thread_count = 1;

    stream = fmt_ctx->streams[video_index];
    if (0 == stream->avg_frame_rate.den) {
      LOGE("videoIndex=%d,videoStream->avg_frame_rate.den = 0", video_index);

      worker_->monitor->video_fps = 25;
    } else {
      worker_->monitor->video_fps =
          stream->avg_frame_rate.num / stream->avg_frame_rate.den;
    }
    // TODO: video resize
    worker_->monitor->video_width = video_codec_ctx->width;
    worker_->monitor->video_height = video_codec_ctx->height;
    worker_->monitor->video_channel = 3;

    if (!worker_->monitor->ParseRecognitionRegion()) {
      LOGE("ParseRecognitionRegion() error");
      return false;
    }

  } else {
    LOGE("av_find_best_stream video error videoIndex=%d",
         worker_->monitor->video_index);
    return false;
  }
  // video end;

  if (worker_->monitor->video_index <= -1) {
    return false;
  }

  connect_count++;

  return true;
}

bool AvPullStream::Reconnect() {
  if (connect_count <= 100) {
    CloseConnect();
    if (Connect()) {
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
  pkt_cs_.release(); //sent a message to wake up the thread and let it terminated itself.
}
void AvPullStream::Read() {
  int continuity_error_count = 0;

  while (worker_->getState()) {
    AVPacketPtr pkt{av_packet_alloc()};
    if (av_read_frame(fmt_ctx.get(), pkt.get()) >= 0) {
      continuity_error_count = 0;
      if (pkt->stream_index == worker_->monitor->video_index) {
        video_pkt_queue_.enqueue(std::move(pkt));
        pkt_cs_.release();
      }
    } else {
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
  AVPacketPtr video_pkt;
  fmt_ctx.reset();
  while (video_pkt_queue_.try_dequeue(video_pkt)) {
    // delete video_pkt;
  }
}
}  // namespace avs_analyzer
