//
// FFmpeg Decoder Implementation
//
#include "decoder_interface.h"
#include "../Utils/log.h"

namespace avs_analyzer {

FFmpegDecoder::~FFmpegDecoder() {
  if (frame_) {
    av_frame_free(&frame_);
  }
  if (codec_ctx_) {
    avcodec_free_context(&codec_ctx_);
  }
}

bool FFmpegDecoder::Initialize(AVCodecParameters* codec_params, AVRational time_base) {
  const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
  if (!codec) {
    LOGE("FFmpegDecoder: could not find decoder for codec_id {}", 
         static_cast<int>(codec_params->codec_id));
    return false;
  }

  codec_ctx_ = avcodec_alloc_context3(codec);
  if (!codec_ctx_) {
    LOGE("FFmpegDecoder: could not allocate codec context");
    return false;
  }

  int ret = avcodec_parameters_to_context(codec_ctx_, codec_params);
  if (ret < 0) {
    LOGE("FFmpegDecoder: avcodec_parameters_to_context failed: {}", ret);
    avcodec_free_context(&codec_ctx_);
    return false;
  }

  codec_ctx_->time_base = time_base;

  ret = avcodec_open2(codec_ctx_, codec, nullptr);
  if (ret < 0) {
    LOGE("FFmpegDecoder: avcodec_open2 failed: {}", ret);
    avcodec_free_context(&codec_ctx_);
    return false;
  }

  frame_ = av_frame_alloc();
  if (!frame_) {
    LOGE("FFmpegDecoder: could not allocate frame");
    avcodec_free_context(&codec_ctx_);
    return false;
  }

  LOGI("FFmpegDecoder: initialized {}x{} fmt={}", 
       codec_ctx_->width, codec_ctx_->height, 
       static_cast<int>(codec_ctx_->pix_fmt));
  return true;
}

bool FFmpegDecoder::SendPacket(AVPacket* packet) {
  if (!codec_ctx_) return false;
  
  int ret = avcodec_send_packet(codec_ctx_, packet);
  if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
    LOGE("FFmpegDecoder: send_packet failed: {}", ret);
    return false;
  }
  return true;
}

AVFrame* FFmpegDecoder::ReceiveFrame() {
  if (!codec_ctx_ || !frame_) return nullptr;

  av_frame_unref(frame_);
  int ret = avcodec_receive_frame(codec_ctx_, frame_);
  
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    pending_ = false;
    return nullptr;
  }
  
  if (ret < 0) {
    LOGE("FFmpegDecoder: receive_frame failed: {}", ret);
    pending_ = false;
    return nullptr;
  }

  pending_ = true;
  return frame_;
}

bool FFmpegDecoder::HasPendingFrames() const {
  return pending_;
}

void FFmpegDecoder::Flush() {
  if (codec_ctx_) {
    avcodec_flush_buffers(codec_ctx_);
  }
  pending_ = false;
}

int FFmpegDecoder::Width() const {
  return codec_ctx_ ? codec_ctx_->width : 0;
}

int FFmpegDecoder::Height() const {
  return codec_ctx_ ? codec_ctx_->height : 0;
}

AVPixelFormat FFmpegDecoder::PixelFormat() const {
  return codec_ctx_ ? codec_ctx_->pix_fmt : AV_PIX_FMT_NONE;
}

}  // namespace avs_analyzer
