#include "frame_recorder.h"
#include <iostream>
#include <stdexcept>


extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

namespace avs_analyzer {
// Helper to log errors
void log_error(const std::string& message, int error_code) {
  static char err_buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(error_code, err_buf, sizeof(err_buf));
  std::cerr << message << ": " << err_buf << " ( " << error_code << ")"
            << std::endl;
}

FrameRecorder::FrameRecorder(const std::string_view filename_prefix, VideoConfig config)
    : m_config(config), file_path_(filename_prefix){}

FrameRecorder::~FrameRecorder() {
  close_output_segment();
}

bool FrameRecorder::initialize() {
  return open_output_segment();  // Start the first segment
}

bool FrameRecorder::open_output_segment() {
  // set up codec
  // 1. Find the H.265 encoder
  const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
  if (!codec) {
    std::cerr << "Codec 'libx265' not found. Please ensure FFmpeg is compiled with support for it."
              << std::endl;
    return false;
  }

  video_codec_ctx_.reset(avcodec_alloc_context3(codec));
  if (!video_codec_ctx_) {
    std::cerr << "Could not allocate video codec context" << std::endl;
    return false;
  }

  // 2. Set encoder parameters
  video_codec_ctx_->width = m_config.width;
  video_codec_ctx_->height = m_config.height;
  video_codec_ctx_->bit_rate = m_config.bitrate;
  video_codec_ctx_->time_base = {1, m_config.fps};
  video_codec_ctx_->framerate = {m_config.fps, 1};
//  video_codec_ctx_->gop_size = m_config.fps;  // One I-frame per second
//  video_codec_ctx_->max_b_frames = 2;
  video_codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

  av_opt_set(video_codec_ctx_->priv_data, "preset", "ultrafast", 0);
  av_opt_set(video_codec_ctx_->priv_data, "tune", "fastdecode", 0);

  // 3. Open the codec
  int ret = avcodec_open2(video_codec_ctx_.get(), codec, nullptr);
  if (ret < 0) {
    log_error("Could not open codec", ret);
    return false;
  }
  // set up output stream
  AVFormatContext* fmt_ctx;
  ret = avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr,
                                           file_path_.c_str());
  output_ctx_.reset(fmt_ctx);
  if (ret < 0) {
    log_error("Could not create output context", ret);
    return false;
  }

  video_stream_ = avformat_new_stream(output_ctx_.get(), nullptr);
  if (!video_stream_) {
    std::cerr << "Failed to create new stream" << std::endl;
    return false;
  }
  video_stream_->id = output_ctx_->nb_streams - 1;
  video_stream_->time_base = video_codec_ctx_->time_base;

  ret = avcodec_parameters_from_context(video_stream_->codecpar,
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

  ret = avformat_write_header(output_ctx_.get(), nullptr);
  if (ret < 0) {
    log_error("Could not write header", ret);
    return false;
  }
  m_segment_start_time = std::chrono::steady_clock::now();
  return true;
}

void FrameRecorder::close_output_segment() {
  if (output_ctx_) {
    // Flush any remaining packets from the encoder
    encode_and_write(nullptr);

    std::cout << "Closing file: " << file_path_ << std::endl;
    av_write_trailer(output_ctx_.get());
    output_ctx_.reset();
  }
}

void FrameRecorder::encode_and_write(AVFrame* frame) {
  int ret = avcodec_send_frame(video_codec_ctx_.get(), frame);
  if (ret < 0) {
    log_error("Error sending frame to encoder", ret);
    return;
  }

  AVPacket* pkt = av_packet_alloc();
  while (true) {
    ret = avcodec_receive_packet(video_codec_ctx_.get(), pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    } else if (ret < 0) {
      log_error("Error receiving packet from encoder", ret);
      break;
    }

    // Rescale timestamps and write the packet
    av_packet_rescale_ts(pkt, video_codec_ctx_->time_base,
                         video_stream_->time_base);
    pkt->stream_index = video_stream_->index;
    av_interleaved_write_frame(output_ctx_.get(), pkt);
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
}

bool FrameRecorder::add_video_frame(AVFrame* frame) {
  frame->pts = pts_++;
  encode_and_write(frame);
  return true;
}

void FrameRecorder::finalize() {
  close_output_segment();
}

}