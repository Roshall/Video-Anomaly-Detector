#include "av_push_stream.h"

#include "Utils/common.h"
#include "Utils/log.h"
#include "analyzer.h"
#include "config.h"
#include "control.h"
#include "frame.h"
#include "worker.h"
extern "C" {
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>

#include "libswscale/swscale.h"
}

namespace avs_analyzer {
AvPushStream::AvPushStream(Worker* worker) : m_worker_(worker) {}

AvPushStream::~AvPushStream() {
  CloseConnect();
}

bool AvPushStream::Connect() {
  std::string in_stream_url = m_worker_->monitor->push_stream_url;
  const int video_width = m_worker_->monitor->video_width;
  const int video_height = m_worker_->monitor->video_height;
  const int video_fps = m_worker_->monitor->video_fps;

  if (avformat_alloc_output_context2(&fmt_ctx, nullptr, "rtsp",
                                     in_stream_url.data()) < 0) {
    LOGI("avformat_alloc_output_context2 error: pushStreamUrl=%s",
         in_stream_url.data());
    return false;
  }

  // init video start
  // NVIDIA: h264_nvenc, hevc_nvenc
  // Intel: h264_qsv, hevc_qsv
  // AMD/VAAPI: h264_vaapi, hevc_vaapi
  // where hevc = h265
  const AVCodec* videoCodec = avcodec_find_encoder_by_name("ibx265");
  if (!videoCodec) {
    LOGI("avcodec_find_encoder error: pushStreamUrl=%s", in_stream_url.data());
    return false;
  }
  video_codec_ctx = avcodec_alloc_context3(videoCodec);
  if (!video_codec_ctx) {
    LOGI("avcodec_alloc_context3 error: pushStreamUrl=%s",
         in_stream_url.data());
    return false;
  }
  // int bit_rate = 300 * 1024 * 8;  //压缩后每秒视频的bit位大小 300kB
  //  constexpr int bit_rate = 40960000;
  // CBR：Constant BitRate - 固定比特率
  //        mVideoCodecCtx->flags |= AV_CODEC_FLAG_QSCALE;
  //        mVideoCodecCtx->bit_rate = bit_rate;
  //        mVideoCodecCtx->rc_min_rate = bit_rate;
  //        mVideoCodecCtx->rc_max_rate = bit_rate;
  //        mVideoCodecCtx->bit_rate_tolerance = bit_rate;
  constexpr int bit_rate = 20480000;

  // VBR
  video_codec_ctx->flags |= AV_CODEC_FLAG_QSCALE;
  video_codec_ctx->rc_min_rate = bit_rate / 2;
  video_codec_ctx->rc_max_rate = bit_rate * 1.5;
  video_codec_ctx->bit_rate = bit_rate;

  //  video_codec_ctx->codec_id = videoCodec->id;
  video_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  //  video_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  video_codec_ctx->width = video_width;
  video_codec_ctx->height = video_height;
  video_codec_ctx->time_base = {1, 25};
  //        mDstVideoCodecCtx->framerate = { mDstVideoFps, 1 };
  video_codec_ctx->gop_size = 30;
  video_codec_ctx->max_b_frames = 0;  // b frame introduce latency, disable it
  video_codec_ctx->thread_count = 3;
  video_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // 添加PPS、SPS
  AVDictionary* video_codec_options = nullptr;

  // H.264
  //  if (video_codec_ctx->codec_id == AV_CODEC_ID_H264) {
  //    //            av_dict_set(&video_codec_options, "profile", "main", 0);
  //    av_dict_set(&video_codec_options, "preset", "superfast", 0);
  //    av_dict_set(&video_codec_options, "tune", "zerolatency", 0);
  //  }
  // H.265
  //  if (video_codec_ctx->codec_id == AV_CODEC_ID_H265) {
  av_dict_set(&video_codec_options, "preset", "ultrafast", 0);
  av_dict_set(&video_codec_options, "tune", "zero-latency", 0);
  //  }
  if (avcodec_open2(video_codec_ctx, videoCodec, &video_codec_options) < 0) {
    LOGI("avcodec_open2 error: pushStreamUrl=%s", in_stream_url.data());
    return false;
  }
  video_stream = avformat_new_stream(fmt_ctx, videoCodec);
  if (!video_stream) {
    LOGI("avformat_new_stream error: pushStreamUrl=%s", in_stream_url.data());
    return false;
  }
  video_stream->id = fmt_ctx->nb_streams - 1;
  // stream的time_base参数非常重要，它表示将现实中的一秒钟分为多少个时间基,
  // 在下面调用avformat_write_header时自动完成
  avcodec_parameters_from_context(video_stream->codecpar, video_codec_ctx);
  video_index = video_stream->id;
  // init video end

  av_dump_format(fmt_ctx, 0, in_stream_url.data(), 1);

  // open output url
  if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&fmt_ctx->pb, in_stream_url.data(), AVIO_FLAG_WRITE) < 0) {
      LOGI("avio_open error: pushStreamUrl=%s", in_stream_url.data());
      return false;
    }
  }

  AVDictionary* fmt_options = nullptr;
  // av_dict_set(&fmt_options, "bufsize", "1024", 0);
  av_dict_set(&fmt_options, "rw_timeout", "30000000",
              0);  // 设置rtmp/http-flv连接超时（单位 us）
  av_dict_set(&fmt_options, "stimeout", "30000000",
              0);  // 设置rtsp连接超时（单位 us）
  av_dict_set(&fmt_options, "rtsp_transport", "tcp", 0);
  //        av_dict_set(&fmt_options, "fflags", "discardcorrupt", 0);

  // av_dict_set(&fmt_options, "muxdelay", "0.1", 0);
  // av_dict_set(&fmt_options, "tune", "zerolatency", 0);

  fmt_ctx->video_codec_id = fmt_ctx->oformat->video_codec;

  if (avformat_write_header(fmt_ctx, &fmt_options) <
      0) {  // 调用该函数会将所有stream的time_base，自动设置一个值，通常是1/90000或1/1000，这表示一秒钟表示的时间基长度
    LOGI("avformat_write_header error: pushStreamUrl=%s", in_stream_url.data());
    return false;
  }

  connect_count++;

  return true;
}
bool AvPushStream::Reconnect() {
  if (connect_count <= 100) {
    CloseConnect();
    return Connect();
  }
  return false;
}
void AvPushStream::CloseConnect() {
  ClearVideoFrameQueue();

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  video_frame_cs_.release(); // in case the consumer is still waiting for frames

  if (fmt_ctx) {
    // 推流需要释放start
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
      avio_close(fmt_ctx->pb);
    }
    // 推流需要释放end

    avformat_free_context(fmt_ctx);
    fmt_ctx = nullptr;
  }

  if (video_codec_ctx) {
    if (video_codec_ctx->extradata) {
      av_free(video_codec_ctx->extradata);
      video_codec_ctx->extradata = nullptr;
    }

    avcodec_close(video_codec_ctx);
    avcodec_free_context(&video_codec_ctx);
    video_codec_ctx = nullptr;
    video_index = -1;
  }
}

void AvPushStream::AddVideoFrame(std::shared_ptr<AVFrame> frame) {
  video_frame_q_.enqueue(std::move(frame));
  video_frame_cs_.release();
}
int AvPushStream::GetVideoFrameQSize() { return video_frame_q_.size_approx(); }

std::shared_ptr<AVFrame> AvPushStream::PopVideoFrame() {
  video_frame_cs_.acquire();
  std::shared_ptr<AVFrame> frame;
  video_frame_q_.try_dequeue(frame);
  return frame;
}
void AvPushStream::ClearVideoFrameQueue() {
  std::shared_ptr<AVFrame> video_frame;
  while (video_frame_q_.try_dequeue(video_frame)) {
    // delete video_frame;
  }
}
void AvPushStream::EncodeVideo() {
  AVPacketPtr pkt{av_packet_alloc()};
  int64_t frameCount = 0;

  while (m_worker_->getState()) {
    if (auto frame = PopVideoFrame()) {
      frame->pts = frameCount;

      if (int ret = avcodec_send_frame(video_codec_ctx, frame.get());
          ret >= 0) {
        if (ret = avcodec_receive_packet(video_codec_ctx, pkt.get());
            ret >= 0) {  // 编码后的视频帧

          pkt->stream_index = video_index;

          pkt->pos = -1;
          av_packet_rescale_ts(pkt.get(), video_codec_ctx->time_base, video_stream->time_base);

          ret = av_interleaved_write_frame(fmt_ctx, pkt.get());
          if (ret < 0) {
            LOGE("av_interleaved_write_frame error : ret=%d", ret);
          }

        } else {
          LOGE("avcodec_receive_packet error : ret=%d", ret);
        }

      } else {
        LOGE("avcodec_send_frame error : ret=%d", ret);
      }

      frameCount++;
    }
  }

  // av_write_trailer(mFmtCtx);//写文件尾

  av_packet_unref(pkt.get());
}
void AvPushStream::EncodeVideoThread(void* arg) {
  static_cast<AvPushStream*>(arg)->EncodeVideo();
}

}  // namespace avs_analyzer
