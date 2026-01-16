//
// Created by lg on 10/12/25.
//

#include "streamer.h"
#include <libavutil/frame.h>


namespace avs_analyzer{

Streamer::Streamer(const std::string& url){
    if (!Init(url)) {
      fmt_ctx_.reset();
      codec_ctx_.reset();
    }
}
Streamer::~Streamer() =default;
bool Streamer::Valid() const {
  return fmt_ctx_ && codec_ctx_;
}
AVFrame* Streamer::NextFrame() {
  while (true) {
    int ret = avcodec_receive_frame(codec_ctx_.get(), frame_.get());
    if (ret == 0) {
      return frame_.get();
    }
    if (ret != AVERROR(EAGAIN)) {
      if (ret == AVERROR_EOF) {
        end_ = true;
        return nullptr;
      }
      avs_analyzer::LOGE_FFMPEG("receive_frame failed", ret);
    }

    // Need more input: read next packet
    av_packet_unref(pkt_.get());
    ret = av_read_frame(fmt_ctx_.get(), pkt_.get());
    if (ret < 0) {
      if (ret == AVERROR_EOF) {
        avcodec_send_packet(codec_ctx_.get(), nullptr);
        continue;
      }
      LOGE_FFMPEG("could not read packet", ret);
      return nullptr;
    }

    if (pkt_->stream_index != stream_->index) {
      continue;
    }

    // Send video packet; if decoder is full, pull a frame and retry next loop
    ret = avcodec_send_packet(codec_ctx_.get(), pkt_.get());
    if (ret == AVERROR(EAGAIN)) {
    } else if (ret < 0) {
      LOGE_FFMPEG("send_packet failed", ret);
    }
  }
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
float Streamer::GetFps() const {
  if (stream_->avg_frame_rate.num && stream_->avg_frame_rate.den) {
    return static_cast<float>(stream_->avg_frame_rate.num) / stream_->avg_frame_rate.den;
  }
  return 25.0f;
}
bool Streamer::Init(const std::string& url) {
  fmt_ctx_.reset(avformat_alloc_context());
  if(!fmt_ctx_) {
    return false;
  }
  int ret;
  auto ctx = fmt_ctx_.release();
  if ((ret = avformat_open_input(&ctx, url.c_str(), nullptr, nullptr)) < 0) {
    LOGE_FFMPEG("Could not open input stream:", ret);
    return false;
  }
  fmt_ctx_.reset(ctx);

  if ((ret = avformat_find_stream_info(fmt_ctx_.get(), nullptr)) < 0) {
    LOGE_FFMPEG("Could not find stream information:", ret);
    return false;
  }

  av_dump_format(fmt_ctx_.get(), 0, url.c_str(), 0);
  LOGI("Successfully opened stream: {}", url);
  const auto index = av_find_best_stream(fmt_ctx_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (index < 0) {
    LOGE_FFMPEG("Could not find video stream", ret);
    return false;
  }
  AVStream* st = fmt_ctx_->streams[index];
  if (st->codecpar->codec_id == AV_CODEC_ID_H264) {
    const uint8_t* ed = st->codecpar->extradata;
    int ed_size = st->codecpar->extradata_size;
    if (!ed || ed_size < 7) {
      LOGE("H264: missing/short extradata (avcC). Did find_stream_info run? size={}", ed_size);
    } else {
      // ISO/IEC 14496-15 'avcC' – NAL length size is (ed[4] & 0x03) + 1
      int nal_len_size = (ed[4] & 0x03) + 1;
      LOGI("H264 avcC present. extradata_size={}, nal_length_size={}", ed_size, nal_len_size);
      if (nal_len_size < 1 || nal_len_size > 4) {
        LOGE("H264: invalid nal_length_size={} in avcC", nal_len_size);
      }
    }
  }
  const auto stream_p = fmt_ctx_->streams[index];
  time_base = stream_p->time_base;
  const auto codec_param = stream_p->codecpar;
  stream_ = stream_p;
  const AVCodec* codec_p;
  if ((codec_p = avcodec_find_decoder(codec_param->codec_id))== nullptr){
    LOGE_FFMPEG("Could not find the codec", ret);
    return false;
  }
  codec_ctx_.reset(avcodec_alloc_context3(codec_p));
  if (!codec_ctx_){
    LOGE_FFMPEG("Could not allocated memory for codec context" , ret);
    return false;
  }

  if (ret = avcodec_parameters_to_context(codec_ctx_.get(), codec_param); ret < 0) {
    LOGE_FFMPEG("Could not copy codec params to codec context:" , ret);
    return false;
  }

  if ((ret = avcodec_open2(codec_ctx_.get(), codec_p, nullptr)) < 0) {
    LOGE_FFMPEG("Could not open codec: " , ret);
    return false;
  }
  pkt_.reset(av_packet_alloc());
  frame_.reset(av_frame_alloc());
  return true;
}
}