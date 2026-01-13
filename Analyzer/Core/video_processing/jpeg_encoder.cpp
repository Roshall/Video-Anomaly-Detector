#include "jpeg_encoder.h"

#include <stdexcept>
#include <fstream>
void avs_analyzer::JPEGEncoder::ensureConvFrame(int w, int h, AVPixelFormat fmt) {
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

void avs_analyzer::JPEGEncoder::initCodec(int width, int height) {
  std::scoped_lock lock(mutex_);
  auto codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!codec) throw std::runtime_error("MJPEG codec not found");

  ctx.reset(avcodec_alloc_context3(codec));
  if (!ctx) throw std::runtime_error("Failed to allocate codec context");

  ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  ctx->width  = width;
  ctx->height = height;
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;         // modern way (no YUVJ*)
//    ctx->color_range = AVCOL_RANGE_JPEG;       // full-range (JPEG)
  // ctx->colorspace   = AVCOL_SPC_BT470BG;   // optional

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
    LOGE_FFMPEG("Failed to open MJPEG encoder", ret);
    throw std::runtime_error("Failed to open MJPEG encoder");
  }

  current_width = width;
  current_height = height;
  current_src_fmt = AV_PIX_FMT_NONE; // force sws re-eval on first call
}

avs_analyzer::JPEGEncoder::JPEGEncoder(int initial_width, int initial_height, int initial_quality_q)
    : quality_q(initial_quality_q) {
  initCodec(initial_width, initial_height);
}

void avs_analyzer::JPEGEncoder::setBitRate(int new_bit_rate) {
  std::scoped_lock lock(mutex_);
  bit_rate = new_bit_rate;
  if (quality_q < 0 && current_width > 0) initCodec(current_width, current_height);
}

void avs_analyzer::JPEGEncoder::setQualityQ(int q /*2..31, lower is better*/) {
  if (q < 2 || q > 31) throw std::invalid_argument("quality q must be in [2,31]");
  std::scoped_lock lock(mutex_);
  quality_q = q;
  if (current_width > 0) initCodec(current_width, current_height);
}
std::pair<int, int> avs_analyzer::JPEGEncoder::saveFrame(
    AVFrame *in, const std::string &filename) {
  if (!in) return {AVERROR(EINVAL), 0};
  std::scoped_lock lock(mutex_);

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
      sws_ctx.reset(sws_getContext(in->width, in->height,
                                   (AVPixelFormat)in->format, in->width,
                                   in->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                   nullptr, nullptr, nullptr));
      if (!sws_ctx) return {AVERROR(ENOMEM), 0};
      current_src_fmt = (AVPixelFormat)in->format;
    }

    ensureConvFrame(in->width, in->height, AV_PIX_FMT_YUV420P);

    // Make sure destination is writable
    if (av_frame_make_writable(conv_frame.get()) < 0)
      return {AVERROR(EINVAL), 0};

    sws_scale(sws_ctx.get(), in->data, in->linesize, 0, in->height,
              conv_frame->data, conv_frame->linesize);

    conv_frame->color_range = AVCOL_RANGE_JPEG;
    // send converted frame
    src = conv_frame.get();
  }

  // If using global Q-scale, you can set per-frame quality too
  if ((ctx->flags & AV_CODEC_FLAG_QSCALE) && quality_q >= 0) {
    src->quality = quality_q * FF_QP2LAMBDA;
  }

  int ret = avcodec_send_frame(ctx.get(), src);
  if (ret < 0) return {ret, 0};

  auto pkt = AVPacketPtr(av_packet_alloc());
  if (!pkt) return {AVERROR(ENOMEM), 0};

  std::ofstream out(filename, std::ios::binary);
  if (!out.is_open()) {
    return {AVERROR(errno), 0};
  }

  int total_size = 0;
  // Drain all packets for this frame
  while (true) {
    ret = avcodec_receive_packet(ctx.get(), pkt.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) {
      return {ret, 0};
    }
    out.write(reinterpret_cast<const char *>(pkt->data), pkt->size);
    if (!out) {
      return {AVERROR(EIO), 0};
    }
    total_size += pkt->size;
    av_packet_unref(pkt.get());
  }
  out.close();
  return {0, total_size};
}
