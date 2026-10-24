#include "pipeline/ffmpeg_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "Utils/log.h"

namespace avs_analyzer::pipeline {
namespace {

PixelFormat ToPixelFormat(AVPixelFormat f) noexcept {
  switch (f) {
    case AV_PIX_FMT_NV12: return PixelFormat::kNv12;
    case AV_PIX_FMT_NV21: return PixelFormat::kNv21;
    case AV_PIX_FMT_YUV420P: return PixelFormat::kYuv420p;
    case AV_PIX_FMT_BGR24: return PixelFormat::kBgr24;
    case AV_PIX_FMT_RGB24: return PixelFormat::kRgb24;
    default: return PixelFormat::kUnknown;
  }
}

}  // namespace

struct FfmpegDecoder::Impl {
  AVFormatContext* fmt{nullptr};
  AVCodecContext* codec{nullptr};
  AVFrame* frame{nullptr};
  AVPacket* pkt{nullptr};
  SwsContext* sws{nullptr};
  int video_stream{-1};
  PixelFormat pix{PixelFormat::kUnknown};

  ~Impl() { release(); }

  void release() {
    if (sws) {
      sws_freeContext(sws);
      sws = nullptr;
    }
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    if (codec) avcodec_free_context(&codec);
    if (fmt) avformat_close_input(&fmt);
    video_stream = -1;
    pix = PixelFormat::kUnknown;
  }

  // 把刚解出的 frame 转成 BGR；sws 上下文按分辨率缓存，避免每帧重建。
  bool ToBgr(cv::Mat& out) {
    const int w = frame->width;
    const int h = frame->height;
    if (w <= 0 || h <= 0) return false;

    out.create(h, w, CV_8UC3);
    sws = sws_getCachedContext(sws, w, h, static_cast<AVPixelFormat>(frame->format),
                               w, h, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr,
                               nullptr, nullptr);
    if (!sws) {
      LOGE("FfmpegDecoder: sws_getCachedContext 失败");
      return false;
    }

    const std::uint8_t* src[4] = {frame->data[0], frame->data[1],
                                  frame->data[2], frame->data[3]};
    int src_stride[4] = {frame->linesize[0], frame->linesize[1],
                         frame->linesize[2], frame->linesize[3]};
    std::uint8_t* dst[1] = {out.data};
    int dst_stride[1] = {static_cast<int>(out.step)};

    return sws_scale(sws, src, src_stride, 0, h, dst, dst_stride) > 0;
  }
};

FfmpegDecoder::FfmpegDecoder() : impl_(std::make_unique<Impl>()) {}
FfmpegDecoder::~FfmpegDecoder() = default;

bool FfmpegDecoder::Open(const StreamDesc& desc) {
  auto& im = *impl_;
  im.release();

  // RTSP 走 TCP 并设超时：弱网下 avformat_open_input 否则可能长时间挂住。
  AVDictionary* opts = nullptr;
  if (!desc.is_file) {
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);
  }

  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, desc.url.c_str(), nullptr, &opts) < 0) {
    LOGE("FfmpegDecoder: avformat_open_input 失败 url={}", desc.url);
    av_dict_free(&opts);
    return false;
  }
  av_dict_free(&opts);

  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    LOGE("FfmpegDecoder: avformat_find_stream_info 失败 url={}", desc.url);
    avformat_close_input(&fmt);
    return false;
  }

  const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vs < 0) {
    LOGE("FfmpegDecoder: 未找到视频流 url={}", desc.url);
    avformat_close_input(&fmt);
    return false;
  }

  const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
  if (!dec) {
    LOGE("FfmpegDecoder: 找不到对应解码器 url={}", desc.url);
    avformat_close_input(&fmt);
    return false;
  }

  AVCodecContext* codec = avcodec_alloc_context3(dec);
  if (!codec || avcodec_parameters_to_context(codec, fmt->streams[vs]->codecpar) < 0) {
    LOGE("FfmpegDecoder: 解码器上下文初始化失败 url={}", desc.url);
    avcodec_free_context(&codec);
    avformat_close_input(&fmt);
    return false;
  }
  if (avcodec_open2(codec, dec, nullptr) < 0) {
    LOGE("FfmpegDecoder: avcodec_open2 失败 url={}", desc.url);
    avcodec_free_context(&codec);
    avformat_close_input(&fmt);
    return false;
  }

  im.fmt = fmt;
  im.codec = codec;
  im.video_stream = vs;
  im.frame = av_frame_alloc();
  im.pkt = av_packet_alloc();
  im.pix = ToPixelFormat(codec->pix_fmt);
  if (!im.frame || !im.pkt) {
    LOGE("FfmpegDecoder: 帧/包分配失败 url={}", desc.url);
    im.release();
    return false;
  }

  LOGI("FfmpegDecoder: 已打开 {}x{} url={}", codec->width, codec->height, desc.url);
  return true;
}

void FfmpegDecoder::Close() { impl_->release(); }

bool FfmpegDecoder::IsOpen() const noexcept { return impl_->codec != nullptr; }

int FfmpegDecoder::Width() const noexcept {
  return impl_->codec ? impl_->codec->width : 0;
}

int FfmpegDecoder::Height() const noexcept {
  return impl_->codec ? impl_->codec->height : 0;
}

PixelFormat FfmpegDecoder::pixel_format() const noexcept { return impl_->pix; }

bool FfmpegDecoder::ReceiveFrameTo(cv::Mat& bgr) {
  auto& im = *impl_;
  if (!im.fmt || !im.codec || !im.frame || !im.pkt) return false;

  for (;;) {
    const int ret = avcodec_receive_frame(im.codec, im.frame);
    if (ret == 0) return im.ToBgr(bgr);

    if (ret == AVERROR(EAGAIN)) {
      // 解码器要更多输入：读一个包喂进去。
      const int pr = av_read_frame(im.fmt, im.pkt);
      if (pr < 0) return false;  // EOF 或读错误
      if (im.pkt->stream_index != im.video_stream) {
        av_packet_unref(im.pkt);
        continue;
      }
      const int sr = avcodec_send_packet(im.codec, im.pkt);
      av_packet_unref(im.pkt);
      if (sr < 0 && sr != AVERROR(EAGAIN)) {
        LOGW("FfmpegDecoder: avcodec_send_packet 失败 {}", sr);
        return false;
      }
      continue;
    }
    return false;  // AVERROR_EOF 或其它错误
  }
}

}  // namespace avs_analyzer::pipeline
