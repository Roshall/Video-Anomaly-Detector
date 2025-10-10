//
// Created by lg on 10/10/25.
//

#ifndef ANALYZER_JPEG_ENCODER_H
#define ANALYZER_JPEG_ENCODER_H
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <stdexcept>
#include <string>
#include <cstdio>

#include <helper.h>

namespace avs_analyzer {

class JPEGEncoder {
 private:
  AVCodecContextPtr ctx = nullptr;
  SwsContextPtr sws_ctx = nullptr;
  int64_t pts_;

  int current_width = 0;
  int current_height = 0;
  AVPixelFormat current_src_fmt = AV_PIX_FMT_NONE;

  // Quality controls
  int bit_rate = 4'000'000;     // optional
  int quality_q = -1;           // 2..31 (lower = better). If <0, use bitrate.

  AVFramePtr conv_frame = nullptr; // cached conversion frame when needed

  void ensureConvFrame(int w, int h, AVPixelFormat fmt) {
    if (!conv_frame) {
      conv_frame.reset(av_frame_alloc());
      if (!conv_frame) throw std::runtime_error("Failed to alloc conv_frame");
      conv_frame->width = w;
      conv_frame->height = h;
      conv_frame->format = fmt;
      if (av_frame_get_buffer(conv_frame.get(), 32) < 0)
        throw std::runtime_error("Failed to alloc buffer for conv_frame");
    }
  }

  void initCodec(int width, int height) {
    auto codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) throw std::runtime_error("MJPEG codec not found");

    ctx.reset(avcodec_alloc_context3(codec));
    if (!ctx) throw std::runtime_error("Failed to allocate codec context");

    ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->width  = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;         // modern way (no YUVJ*)
//    ctx->color_range = AVCOL_RANGE_JPEG;       // full-range (JPEG)
    // ctx->colorspace   = AVCOL_SPC_BT470BG;   // optional, pick correct one for your source

    ctx->time_base = {1, 25};
    ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    if (quality_q >= 0) {
      ctx->flags |= AV_CODEC_FLAG_QSCALE;
      ctx->qmin = ctx->qmax = quality_q;     // narrow quant range for consistency
    } else {
      ctx->bit_rate = bit_rate;              // fallback to bitrate mode
    }

    // Intra-only codec, no gop/bframes needed
    int ret;
    if (ret = avcodec_open2(ctx.get(), codec, nullptr); ret < 0){
      log_error("Failed to open MJPEG encoder", ret);
      throw std::runtime_error("Failed to open MJPEG encoder");
    }

    current_width = width;
    current_height = height;
    current_src_fmt = AV_PIX_FMT_NONE; // force sws re-eval on first call
  }

 public:
  JPEGEncoder(int initial_width, int initial_height, int initial_quality_q = -1)
      : quality_q(initial_quality_q) {
    initCodec(initial_width, initial_height);
  }

  void setBitRate(int new_bit_rate) {
    bit_rate = new_bit_rate;
    if (quality_q < 0 && current_width > 0) initCodec(current_width, current_height);
  }

  void setQualityQ(int q /*2..31, lower is better*/) {
    if (q < 2 || q > 31) throw std::invalid_argument("quality q must be in [2,31]");
    quality_q = q;
    if (current_width > 0) initCodec(current_width, current_height);
  }

  int saveFrame(AVFrame *in, const std::string &filename) {
    if (!in) return AVERROR(EINVAL);

    // Reinit encoder if size changed
    if (in->width != current_width || in->height != current_height) {
      initCodec(in->width, in->height);
    }

    in->pts = pts_++;

    AVFrame *src = in;

    // Ensure full-range tagging on the frame going to encoder
    // (Encoders typically read from the frame, not just ctx)
    src->color_range = AVCOL_RANGE_JPEG;

    // Convert if needed
    if (in->format != AV_PIX_FMT_YUV420P) {
      // (Re)build SWS context only when src fmt/size changed
      if (!sws_ctx || current_src_fmt != (AVPixelFormat)in->format) {
        sws_ctx.reset(sws_getContext(
            in->width, in->height, (AVPixelFormat)in->format,
            in->width, in->height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        ));
        if (!sws_ctx) return AVERROR(ENOMEM);
        current_src_fmt = (AVPixelFormat)in->format;
      }

      ensureConvFrame(in->width, in->height, AV_PIX_FMT_YUV420P);

      // Make sure destination is writable
      if (av_frame_make_writable(conv_frame.get()) < 0) return AVERROR(EINVAL);

      sws_scale(sws_ctx.get(),
                in->data, in->linesize,
                0, in->height,
                conv_frame->data, conv_frame->linesize);

      conv_frame->color_range = AVCOL_RANGE_JPEG;
    }

    // If using global Q-scale, you can set per-frame quality too
    if ((ctx->flags & AV_CODEC_FLAG_QSCALE) && quality_q >= 0) {
      conv_frame->quality = quality_q * FF_QP2LAMBDA;
    }

    int ret = avcodec_send_frame(ctx.get(), src);
    if (ret < 0) return ret;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return AVERROR(ENOMEM);

    FILE *f = fopen(filename.c_str(), "wb");
    if (!f) {
      av_packet_free(&pkt);
      return AVERROR(errno);
    }

    // Drain all packets for this frame
    while (true) {
      ret = avcodec_receive_packet(ctx.get(), pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
      if (ret < 0) {
        fclose(f);
        av_packet_free(&pkt);
        return ret;
      }
      fwrite(pkt->data, 1, pkt->size, f);
      av_packet_unref(pkt);
    }

    fclose(f);
    av_packet_free(&pkt);
    return 0;
  }
};
}
#endif  // ANALYZER_JPEG_ENCODER_H
