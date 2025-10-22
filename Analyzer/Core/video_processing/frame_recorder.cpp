#include "frame_recorder.h"

#include <iostream>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace avs_analyzer {

Recorder::Recorder(VideoConfig config)
    : config(config), reused_pkt_(av_packet_alloc()) {
  if (!reused_pkt_) {
    log_error("Failed to allocate packet", AVERROR(ENOMEM));
  }
}

Recorder::~Recorder() {CloseOutputSegment(); }

bool Recorder::Initialize(std::string_view file_path, bool file) {
  file_path_ = file_path;
  CodeConfig conf{};

  if (!file) {
    AVDictionaryPtr fmt_options{};
    fmt_options.set("rw_timeout", "30000000", 0);  // 设置rtmp/http-flv连接超时
    fmt_options.set("stimeout", "30000000", 0);  // ns
    fmt_options.set("rtsp_transport", "tcp", 0);
    conf = {1, 0, "25", "superfast", "rtsp", std::move(fmt_options)};
  }
  return OpenOutputSegment(conf);  // Start the first segment
}

bool Recorder::OpenOutputSegment(const CodeConfig& cconfig) {
  // set up codec
  // 1. Find the H.265 encoder
  const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
  if (!codec) {
    std::cerr << "Codec 'libx264' not found. Please ensure FFmpeg is compiled "
                 "with support for it."
              << std::endl;
    return false;
  }

  video_codec_ctx_.reset(avcodec_alloc_context3(codec));
  if (!video_codec_ctx_) {
    std::cerr << "Could not allocate video codec context" << std::endl;
    return false;
  }

  // 2. Set encoder parameters
  video_codec_ctx_->width = config.width;
  video_codec_ctx_->height = config.height;
  //  video_codec_ctx_->bit_rate = config.bitrate;
  video_codec_ctx_->time_base = {1, config.fps};
    video_codec_ctx_->framerate = {config.fps, 1};
  video_codec_ctx_->gop_size = config.fps * cconfig.gop_size_scale;
  video_codec_ctx_->max_b_frames = cconfig.max_b_frame;
  video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

  av_opt_set(video_codec_ctx_->priv_data, "crf", cconfig.crf.c_str(), 0);
  av_opt_set(video_codec_ctx_->priv_data, "preset", cconfig.preset.c_str(), 0);

  video_codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  // 3. Open the codec
  int ret = avcodec_open2(video_codec_ctx_.get(), codec, nullptr);
  if (ret < 0) {
    log_error("Could not open codec", ret);
    return false;
  }
  // set up output stream
  AVFormatContext* fmt_ctx;
  ret = avformat_alloc_output_context2(&fmt_ctx, nullptr, cconfig.format_name,
                                       file_path_.c_str());
  if (ret < 0) {
    log_error("Could not create output context", ret);
    return false;
  }
  output_ctx_.reset(fmt_ctx);

  stream_ = avformat_new_stream(output_ctx_.get(), nullptr);
  if (!stream_) {
    std::cerr << "Failed to create new stream" << std::endl;
    return false;
  }
  stream_->id = output_ctx_->nb_streams - 1;
  stream_->time_base = video_codec_ctx_->time_base;

  ret = avcodec_parameters_from_context(stream_->codecpar,
                                        video_codec_ctx_.get());
  if (ret < 0) {
    log_error("Could not copy codec parameters", ret);
    return false;
  }

  if (!(output_ctx_->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&output_ctx_->pb, file_path_.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
      log_error("Could not open output file", ret);
      return false;
    }
  }

  ret = avformat_write_header(output_ctx_.get(), cconfig.fmt_options.addr());

  if (ret < 0) {
    log_error("Could not write header", ret);
    return false;
  }
  initialized_ = true;
  return true;
}

void Recorder::CloseOutputSegment() {
  if ( initialized_ && output_ctx_) {
    // Flush any remaining packets from the encoder
    EncodeAndWrite(nullptr);

    std::cout << "Closing file: " << file_path_ << std::endl;
    av_write_trailer(output_ctx_.get());
    output_ctx_.reset();
  }
}

bool Recorder::EncodeAndWrite(AVFrame* frame) {
  int ret = avcodec_send_frame(video_codec_ctx_.get(), frame);
  if (ret < 0) {
    log_error("Error sending frame to encoder", ret);
    return false;
  }

  bool get_frame{true};
  while (true) {
    ret = avcodec_receive_packet(video_codec_ctx_.get(), reused_pkt_.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    } else if (ret < 0) {
      get_frame = false;
      log_error("Error receiving packet from encoder", ret);
      break;
    }
    reused_pkt_->stream_index = stream_->index;
    if (ret = av_interleaved_write_frame(output_ctx_.get(), reused_pkt_.get());
        ret < 0) {
      get_frame = false;
      log_error("Error writing frame to output", ret);
      break;
    }
    av_packet_unref(reused_pkt_.get());
  }
  return get_frame;
}

bool Recorder::AddVideoFrame(AVFrame* frame) {
  // frame.time_base must set to input stream's time base
  frame->pts = av_rescale_q(frame->best_effort_timestamp, frame->time_base,
                            {1, 90000});
  return EncodeAndWrite(frame);
}
}  // namespace avs_analyzer