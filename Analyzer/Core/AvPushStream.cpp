#include "AvPushStream.h"

#include "Analyzer.h"
#include "Config.h"
#include "Control.h"
#include "Frame.h"
#include "Utils/Common.h"
#include "Utils/Log.h"
#include "Worker.h"
extern "C" {
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>

#include "libswscale/swscale.h"
}

namespace avs_analyzer {
AvPushStream::AvPushStream(Worker* worker) : m_worker_(worker) { LOGI(""); }

AvPushStream::~AvPushStream() {
  LOGI("");
  CloseConnect();
  ClearVideoFrameQueue();
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
  const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
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
  constexpr int bit_rate = 40960000;
  // CBR：Constant BitRate - 固定比特率
  //        mVideoCodecCtx->flags |= AV_CODEC_FLAG_QSCALE;
  //        mVideoCodecCtx->bit_rate = bit_rate;
  //        mVideoCodecCtx->rc_min_rate = bit_rate;
  //        mVideoCodecCtx->rc_max_rate = bit_rate;
  //        mVideoCodecCtx->bit_rate_tolerance = bit_rate;

  // VBR
  video_codec_ctx->flags |= AV_CODEC_FLAG_QSCALE;
  video_codec_ctx->rc_min_rate = bit_rate / 2;
  video_codec_ctx->rc_max_rate = bit_rate / 2 + bit_rate;
  video_codec_ctx->bit_rate = bit_rate;

  // ABR：Average Bitrate - 平均码率
  //        mDstVimVideoCodecCtxdeoCodecCtx->bit_rate = bit_rate;

  video_codec_ctx->codec_id = videoCodec->id;
  video_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  video_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  video_codec_ctx->width = video_width;
  video_codec_ctx->height = video_height;
  video_codec_ctx->time_base = {1, video_fps};
  //        mDstVideoCodecCtx->framerate = { mDstVideoFps, 1 };
  video_codec_ctx->gop_size = 1;
  video_codec_ctx->max_b_frames = 0;
  video_codec_ctx->thread_count = 5;
  video_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // 添加PPS、SPS
  AVDictionary* video_codec_options = NULL;

  // H.264
  if (video_codec_ctx->codec_id == AV_CODEC_ID_H264) {
    //            av_dict_set(&video_codec_options, "profile", "main", 0);
    av_dict_set(&video_codec_options, "preset", "superfast", 0);
    av_dict_set(&video_codec_options, "tune", "zerolatency", 0);
  }
  // H.265
  if (video_codec_ctx->codec_id == AV_CODEC_ID_H265) {
    av_dict_set(&video_codec_options, "preset", "ultrafast", 0);
    av_dict_set(&video_codec_options, "tune", "zero-latency", 0);
  }
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
  LOGI("");

  ClearVideoFrameQueue();

  std::this_thread::sleep_for(std::chrono::milliseconds(1));

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

void AvPushStream::AddVideoFrame(Frame* frame) {
  video_frame_q_mtx_.lock();
  video_frame_q_.push(frame);
  video_frame_q_mtx_.unlock();
}
int AvPushStream::GetVideoFrameQSize() {
  int size = 0;
  video_frame_q_mtx_.lock();
  size = video_frame_q_.size();
  video_frame_q_mtx_.unlock();

  return size;
}

bool AvPushStream::GetVideoFrame(Frame*& frame) {
  video_frame_q_mtx_.lock();

  if (!video_frame_q_.empty()) {
    frame = video_frame_q_.front();
    video_frame_q_.pop();
    video_frame_q_mtx_.unlock();
    return true;
  }
  video_frame_q_mtx_.unlock();
  return false;
}
void AvPushStream::ClearVideoFrameQueue() {
  video_frame_q_mtx_.lock();
  while (!video_frame_q_.empty()) {
    Frame* frame = video_frame_q_.front();
    video_frame_q_.pop();
    m_worker_->video_frame_pool->giveBack(frame);
  }
  video_frame_q_mtx_.unlock();
}
void AvPushStream::EncodeVideo() {
  const int width = m_worker_->monitor->video_width;
  const int height = m_worker_->monitor->video_height;

  Frame* video_frame = nullptr;  // 未编码的视频帧（bgr格式）

  AVFrame* frame_yuv420p = av_frame_alloc();
  frame_yuv420p->format = video_codec_ctx->pix_fmt;
  frame_yuv420p->width = width;
  frame_yuv420p->height = height;

  int frame_yuv420p_buff_size =
      av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width, height, 1);
  auto frame_yuv420p_buff = static_cast<uint8_t*>(av_malloc(frame_yuv420p_buff_size));
  av_image_fill_arrays(frame_yuv420p->data, frame_yuv420p->linesize,
                       frame_yuv420p_buff, AV_PIX_FMT_YUV420P, width, height,
                       1);

  AVPacket* pkt = av_packet_alloc();  // 编码后的视频帧
  int64_t encode_success_count{};
  int64_t frameCount = 0;

  int64_t t1 = 0;
  int64_t t2 = 0;
  int ret = -1;
  while (m_worker_->getState()) {
    if (GetVideoFrame(video_frame)) {
      // frame_bgr 转  frame_yuv420p
      Bgr24ToYuv420P(video_frame->GetBuf(), width, height, frame_yuv420p_buff);
      m_worker_->video_frame_pool->giveBack(video_frame);

      frame_yuv420p->pts = frame_yuv420p->pkt_dts = av_rescale_q_rnd(
          frameCount, video_codec_ctx->time_base, video_stream->time_base,
          (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));

      frame_yuv420p->pkt_duration = av_rescale_q_rnd(
          1, video_codec_ctx->time_base, video_stream->time_base,
          (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));

      frame_yuv420p->pkt_pos = -1;

      t1 = getCurTime();
      ret = avcodec_send_frame(video_codec_ctx, frame_yuv420p);
      if (ret >= 0) {
        ret = avcodec_receive_packet(video_codec_ctx, pkt);
        if (ret >= 0) {
          t2 = getCurTime();
          encode_success_count++;

          // LOGI("encode 1 frame spend：%lld(ms),frameCount=%lld,
          // encodeSuccessCount = %lld, frameQSize=%d,ret=%d",
          //     (t2 - t1), frameCount, encodeSuccessCount, frameQSize, ret);
          pkt->stream_index = video_index;

          pkt->pos = -1;
          pkt->duration = frame_yuv420p->pkt_duration;

          ret = av_interleaved_write_frame(fmt_ctx, pkt);
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
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // av_write_trailer(mFmtCtx);//写文件尾

  av_packet_unref(pkt);

  av_free(frame_yuv420p_buff);

  av_frame_free(&frame_yuv420p);
  // av_frame_unref(frame_yuv420p);
}
void AvPushStream::EncodeVideoThread(void* arg) {
  static_cast<AvPushStream*>(arg)->EncodeVideo();
}

unsigned char AvPushStream::ClipValue(unsigned char x, unsigned char min_val,
                                      unsigned char max_val) {
  if (x > max_val) {
    return max_val;
  }
  if (x < min_val) {
    return min_val;
  }
  return x;
}

bool AvPushStream::Bgr24ToYuv420P(unsigned char* bgrBuf, int w, int h,
                                  unsigned char* yuvBuf) {
  memset(yuvBuf, 0, w * h * 3 / 2);
  unsigned char* ptrY = yuvBuf;
  unsigned char* ptrU = yuvBuf + w * h;
  unsigned char* ptrV = ptrU + (w * h * 1 / 4);

  for (int j = 0; j < h; ++j) {
    const unsigned char* ptrRGB = bgrBuf + w * j * 3;
    for (int i = 0; i < w; i++) {
      unsigned char b = *(ptrRGB++);
      unsigned char g = *(ptrRGB++);
      unsigned char r = *(ptrRGB++);

      unsigned char y =
          static_cast<unsigned char>((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
      unsigned char u =
          static_cast<unsigned char>((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
      unsigned char v =
          static_cast<unsigned char>((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
      *(ptrY++) = ClipValue(y, 0, 255);
      if (j % 2 == 0 && i % 2 == 0) {
        *(ptrU++) = ClipValue(u, 0, 255);
      } else {
        if (i % 2 == 0) {
          *(ptrV++) = ClipValue(v, 0, 255);
        }
      }
    }
  }
  return true;
}
}  // namespace avs_analyzer
