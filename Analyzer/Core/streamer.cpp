//
// Created by lg on 10/12/25.
//

#include "streamer.h"


namespace avs_analyzer{

Streamer::Streamer(const std::string& url){
    Init(url);
}
Streamer::~Streamer() =default;
bool Streamer::Valid() {
  return fmt_ctx_ && codec_ctx_;
}
AVFrame* Streamer::NextFrame() {
  av_packet_unref(pkt_.get());
  int ret;
  if ((ret = av_read_frame(fmt_ctx_.get(), pkt_.get())) < 0) {
    if (ret == AVERROR_EOF) {
      end_ = true;
      return nullptr;
    }
    avs_analyzer::log_error("could not read package", ret);
    return nullptr;
  }
  if ((ret = avcodec_send_packet(codec_ctx_.get(), pkt_.get())) < 0) {
    log_error("Could not send packet to the decoder", ret);
    return nullptr;
  }
  av_frame_unref(frame_.get());
  ret = avcodec_receive_frame(codec_ctx_.get(), frame_.get());
  if (ret == AVERROR_EOF) {
    end_ = true;
    return nullptr;
  } else if (ret < 0) {
    avs_analyzer::log_error("cound not receive frame from the decoder", ret);
    return nullptr;
  }

  return frame_.get();
}
bool Streamer::EoF() const {
  return end_;
}

int Streamer::width() const {
  return codec_ctx_->width;
}
int Streamer::height() const {
  return codec_ctx_->height;
}
AVPixelFormat Streamer::PixelFormat() const { return codec_ctx_->pix_fmt; }
bool Streamer::Init(const std::string& url) {
  fmt_ctx_.reset(avformat_alloc_context());
  if(!fmt_ctx_) {
    return false;
  }
  int ret;
  auto ctx = fmt_ctx_.get();
  if ((ret = avformat_open_input(&ctx, url.c_str(), nullptr, nullptr)) < 0) {
    log_error("Could not open input stream:", ret);
    return false;
  }

  if ((ret = avformat_find_stream_info(fmt_ctx_.get(), nullptr)) < 0) {
    log_error("Could not find stream information:", ret);
    return false;
  }

  av_dump_format(fmt_ctx_.get(), 0, url.c_str(), 0);
  log_error("Successfully opened stream.", ret);
  auto index = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (index < 0) {
    log_error("Could not find video stream", ret);
    return false;
  }
  auto stream_p = fmt_ctx_->streams[index];
  time_base = stream_p->time_base;
  fps = stream_p->avg_frame_rate.num / stream_p->avg_frame_rate.den;
  auto codec_param = stream_p->codecpar;
  const AVCodec* codec_p;
  if ((codec_p = avcodec_find_decoder(codec_param->codec_id))== nullptr){
    log_error("Could not find the codec", ret);
    return false;
  }
  codec_ctx_.reset(avcodec_alloc_context3(codec_p));
  if (!codec_ctx_){
    log_error("Could not allocated memory for codec context" , ret);
    return false;
  }

  if (ret = avcodec_parameters_to_context(codec_ctx_.get(), codec_param); ret < 0) {
    log_error("Could not copy codec params to codec context:" , ret);
    return false;
  }

  if ((ret = avcodec_open2(codec_ctx_.get(), codec_p, nullptr)) < 0) {
    log_error("Could not open codec: " , ret);
    return false;
  }
  pkt_.reset(av_packet_alloc());
  frame_.reset(av_frame_alloc());
  return true;
}
}