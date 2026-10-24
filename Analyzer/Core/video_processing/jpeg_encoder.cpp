#include "jpeg_encoder.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace avs_analyzer {
namespace {

// sws_getCachedContext frees the old context when the parameters changed.
// `holder` is released first so ownership is not double-freed.
SwsContext* GetCachedSws(SwsContextPtr& holder, int src_w, int src_h,
                         AVPixelFormat src_fmt, int dst_w, int dst_h,
                         AVPixelFormat dst_fmt, int flags) {
  SwsContext* updated =
      sws_getCachedContext(holder.release(), src_w, src_h, src_fmt, dst_w,
                           dst_h, dst_fmt, flags, nullptr, nullptr, nullptr);
  holder.reset(updated);
  return updated;
}

}  // namespace

void avs_analyzer::JPEGEncoder::ensureConvFrame(int w, int h, AVPixelFormat fmt) {
  if (conv_frame && conv_frame->width == w && conv_frame->height == h &&
      conv_frame->format == fmt) {
    return;
  }
  conv_frame.reset(av_frame_alloc());
  if (!conv_frame) throw std::runtime_error("Failed to alloc conv_frame");
  conv_frame->width = w;
  conv_frame->height = h;
  conv_frame->format = fmt;
  if (av_frame_get_buffer(conv_frame.get(), 32) < 0)
    throw std::runtime_error("Failed to alloc buffer for conv_frame");
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

int avs_analyzer::JPEGEncoder::encodeFrameLocked(
    const AVFrame* in, std::vector<uint8_t>& out) {
  if (!in) return AVERROR(EINVAL);
  if (in->width <= 0 || in->height <= 0 || !in->data[0]) {
    return AVERROR(EINVAL);
  }

  // Reinit encoder if size changed
  if (in->width != current_width || in->height != current_height) {
    initCodec(in->width, in->height);
  }

  const int64_t pts = pts_++;
  const AVFrame* src = in;
  // Mutable clone the encoder may annotate (never the caller's frame).
  AVFrame* writable_src = nullptr;
  AVFramePtr cloned;
  if (in->format != AV_PIX_FMT_YUV420P) {
    // (Re)build the SWS context only when src fmt/size changed.
    if (!GetCachedSws(sws_ctx, in->width, in->height,
                      static_cast<AVPixelFormat>(in->format), in->width,
                      in->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR)) {
      return AVERROR(ENOMEM);
    }
    current_src_fmt = static_cast<AVPixelFormat>(in->format);
    ensureConvFrame(in->width, in->height, AV_PIX_FMT_YUV420P);

    // Make sure destination is writable
    if (av_frame_make_writable(conv_frame.get()) < 0) return AVERROR(EINVAL);

    sws_scale(sws_ctx.get(), in->data, in->linesize, 0, in->height,
              conv_frame->data, conv_frame->linesize);

    conv_frame->pts = pts;
    conv_frame->color_range = AVCOL_RANGE_JPEG;
    src = conv_frame.get();
    writable_src = conv_frame.get();
  } else {
    // Shallow clone so metadata changes (pts/range) never leak into frames
    // that are also consumed by the HLS recorder or the push pipeline.
    cloned.reset(av_frame_clone(in));
    if (!cloned) return AVERROR(ENOMEM);
    cloned->pts = pts;
    cloned->color_range = AVCOL_RANGE_JPEG;
    src = cloned.get();
    writable_src = cloned.get();
  }

  // If using global Q-scale, you can set per-frame quality too.
  if (writable_src && (ctx->flags & AV_CODEC_FLAG_QSCALE) && quality_q >= 0) {
    writable_src->quality = quality_q * FF_QP2LAMBDA;
  }

  int ret = avcodec_send_frame(ctx.get(), src);
  if (ret < 0) return ret;

  auto pkt = AVPacketPtr(av_packet_alloc());
  if (!pkt) return AVERROR(ENOMEM);

  // Drain all packets for this frame (MJPEG emits exactly one).
  while (true) {
    ret = avcodec_receive_packet(ctx.get(), pkt.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) return ret;
    out.insert(out.end(), pkt->data, pkt->data + pkt->size);
    av_packet_unref(pkt.get());
  }
  return 0;
}

std::pair<int, int> avs_analyzer::JPEGEncoder::saveFrame(
    AVFrame *in, const std::string &filename) {
  if (!in) return {AVERROR(EINVAL), 0};
  std::scoped_lock lock(mutex_);

  std::vector<uint8_t> encoded;
  const int ret = encodeFrameLocked(in, encoded);
  if (ret < 0) return {ret, 0};

  std::ofstream out(filename, std::ios::binary);
  if (!out.is_open()) {
    return {AVERROR(errno), 0};
  }
  out.write(reinterpret_cast<const char *>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
  if (!out) {
    return {AVERROR(EIO), 0};
  }
  out.close();
  return {0, static_cast<int>(encoded.size())};
}

std::vector<uint8_t> avs_analyzer::JPEGEncoder::EncodeToBuffer(
    const AVFrame* in) {
  std::vector<uint8_t> out;
  if (!in) return out;
  std::scoped_lock lock(mutex_);
  if (encodeFrameLocked(in, out) < 0) {
    out.clear();
  }
  return out;
}

bool avs_analyzer::JPEGEncoder::ensureDecoderLocked() {
  if (dec_ctx) return true;
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
  if (!codec) {
    LOGE("MJPEG decoder not found");
    return false;
  }
  dec_ctx.reset(avcodec_alloc_context3(codec));
  if (!dec_ctx) return false;
  dec_ctx->thread_count = 1;
  const int ret = avcodec_open2(dec_ctx.get(), codec, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("Failed to open MJPEG decoder", ret);
    dec_ctx.reset();
    return false;
  }
  return true;
}

avs_analyzer::AVFramePtr avs_analyzer::JPEGEncoder::DecodeToFrame(
    const std::vector<uint8_t>& data) {
  if (data.empty()) return nullptr;
  std::scoped_lock lock(mutex_);
  if (!ensureDecoderLocked()) return nullptr;

  auto pkt = AVPacketPtr(av_packet_alloc());
  if (!pkt) return nullptr;
  if (av_new_packet(pkt.get(), static_cast<int>(data.size())) < 0) {
    return nullptr;
  }
  std::memcpy(pkt->data, data.data(), data.size());
  pkt->pts = pkt->dts = dec_pts_++;

  int ret = avcodec_send_packet(dec_ctx.get(), pkt.get());
  if (ret < 0) {
    avcodec_flush_buffers(dec_ctx.get());
    return nullptr;
  }

  AVFramePtr frame{av_frame_alloc()};
  if (!frame) return nullptr;
  ret = avcodec_receive_frame(dec_ctx.get(), frame.get());
  if (ret < 0) {
    avcodec_flush_buffers(dec_ctx.get());
    return nullptr;
  }

  if (frame->format == AV_PIX_FMT_YUV420P) {
    return frame;
  }

  // JPEG decoders usually emit full-range YUVJ420P. The HLS/MP4 recorders
  // expect limited-range planar YUV420P.
  SwsContext* sws = GetCachedSws(
      dec_sws_ctx, frame->width, frame->height,
      static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
      AV_PIX_FMT_YUV420P, SWS_BILINEAR);
  if (!sws) return nullptr;

  AVFramePtr converted{av_frame_alloc()};
  if (!converted) return nullptr;
  converted->format = AV_PIX_FMT_YUV420P;
  converted->width = frame->width;
  converted->height = frame->height;
  if (av_frame_get_buffer(converted.get(), 32) < 0) return nullptr;
  if (av_frame_make_writable(converted.get()) < 0) return nullptr;
  sws_scale(dec_sws_ctx.get(), frame->data, frame->linesize, 0, frame->height,
            converted->data, converted->linesize);
  converted->pts = frame->pts;
  converted->best_effort_timestamp = frame->best_effort_timestamp;
  converted->color_range = AVCOL_RANGE_MPEG;
  return converted;
}

}  // namespace avs_analyzer
