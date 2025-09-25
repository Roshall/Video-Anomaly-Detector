#include "AvPullStream.h"
#include "Config.h"
#include "Control.h"
#include "Scheduler.h"
#include "Utils/Log.h"
#include "Worker.h"

namespace avs_analyzer {
AvPullStream::AvPullStream(Worker* worker) : worker_(worker) { LOGI(""); }

AvPullStream::~AvPullStream() {
  LOGI("");

  CloseConnect();
}

bool AvPullStream::Connect() {
  std::string_view stream_url = worker_->monitor->stream_url;
  fmt_ctx = avformat_alloc_context();

  AVDictionary* fmt_options = nullptr;
  av_dict_set(&fmt_options, "rtsp_transport", "tcp",
              0);  // 设置rtsp底层网络协议 tcp or udp
  av_dict_set(&fmt_options, "stimeout", "10000000",
              0);  // 设置rtsp连接超时（单位 us）1秒=1000000
  av_dict_set(&fmt_options, "rw_timeout", "1000000",
              0);  // 设置rtmp/http-flv连接超时（单位 us）
  // av_dict_set(&fmt_options, "timeout", "1000000", 0);//设置udp/http超时（单位
  // us）

  int ret =
      avformat_open_input(&fmt_ctx, stream_url.data(), nullptr, &fmt_options);

  if (ret != 0) {
    LOGE("avformat_open_input error: url=%s ", stream_url.data());
    return false;
  }

  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    LOGE("avformat_find_stream_info error");
    return false;
  }

  // video start
  auto video_index =
      av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  worker_->monitor->video_index = video_index;
  if (video_index > -1) {
    AVCodecParameters* videoCodecPar = fmt_ctx->streams[video_index]->codecpar;

    const AVCodec* video_codec = nullptr;
    if (!video_codec) {
      video_codec = avcodec_find_decoder(videoCodecPar->codec_id);
      if (!video_codec) {
        LOGE("avcodec_find_decoder error");
        return false;
      }
    }

    video_codec_ctx = avcodec_alloc_context3(video_codec);
    if (avcodec_parameters_to_context(video_codec_ctx, videoCodecPar) != 0) {
      LOGE("avcodec_parameters_to_context error");
      return false;
    }
    if (avcodec_open2(video_codec_ctx, video_codec, nullptr) < 0) {
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
void AvPullStream::CloseConnect() {
  LOGI("");

  ClearVideoPktQ();

  std::this_thread::sleep_for(std::chrono::milliseconds(1));

  if (video_codec_ctx) {
    avcodec_close(video_codec_ctx);
    avcodec_free_context(&video_codec_ctx);
    video_codec_ctx = NULL;
    worker_->monitor->video_index = -1;
  }

  if (fmt_ctx) {
    // 拉流不需要释放start
    // if (mFmtCtx && !(mFmtCtx->oformat->flags & AVFMT_NOFILE)) {
    //    avio_close(mFmtCtx->pb);
    //}
    // 拉流不需要释放end
    avformat_close_input(&fmt_ctx);
    avformat_free_context(fmt_ctx);
    fmt_ctx = NULL;
  }
}

bool AvPullStream::PushVideoPkt(const AVPacket& pkt) {
  if (av_packet_make_refcounted((AVPacket*)&pkt) < 0) {
    return false;
  }

  pkt_queue_mtx_.lock();
  pkt_queue_mtx_.unlock();
  video_pkt_queue_.push(pkt);

  return true;
}
bool AvPullStream::GetVideoPkt(AVPacket& pkt, int& pktQSize) {
  pkt_queue_mtx_.lock();

  if (!video_pkt_queue_.empty()) {
    pkt = video_pkt_queue_.front();
    video_pkt_queue_.pop();
    pktQSize = video_pkt_queue_.size();
    pkt_queue_mtx_.unlock();
    return true;

  } else {
    pkt_queue_mtx_.unlock();
    return false;
  }
}
void AvPullStream::ClearVideoPktQ() {
  pkt_queue_mtx_.lock();
  while (!video_pkt_queue_.empty()) {
    AVPacket pkt = video_pkt_queue_.front();
    video_pkt_queue_.pop();

    av_packet_unref(&pkt);
  }
  pkt_queue_mtx_.unlock();
}
void AvPullStream::HandleRead() {
  int continuity_error_count = 0;

  AVPacket pkt;
  while (worker_->getState()) {
    if (av_read_frame(fmt_ctx, &pkt) >= 0) {
      continuity_error_count = 0;

      if (pkt.stream_index == worker_->monitor->video_index) {
        //                    mVideoPktQ_mtx.lock();
        //                    int pktQSize = mVideoPktQ.size();
        //                    mVideoPktQ_mtx.unlock();
        //                    if(pktQSize > 0){
        //                        this->clearVideoPktQueue();
        //                    }

        PushVideoPkt(pkt);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        // av_free_packet(&pkt);//过时
        av_packet_unref(&pkt);
      }
    } else {
      // av_free_packet(&pkt);//过时
      av_packet_unref(&pkt);
      continuity_error_count++;
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
void AvPullStream::ReadThread(void* arg) {
  AvPullStream* pullStream = (AvPullStream*)arg;
  pullStream->HandleRead();
}
}  // namespace avs_analyzer
