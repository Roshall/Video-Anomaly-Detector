//
// Created by lg on 10/2/25.
//
#include "test_helper.h"

#include <format>

#include "frame_recorder.h"
#include "helper.h"

Streamer::Streamer(const std::string& url): fmt_ctx_(avformat_alloc_context()){
    int ret;
    constexpr int err_char_size{64};
    char errbuf[err_char_size]{};
    if ((ret = avformat_open_input(&fmt_ctx_, url.c_str(), nullptr, nullptr)) < 0) {
      std::cerr << "Could not open input stream:" << av_make_error_string(errbuf, err_char_size, ret) << std::endl;
      return;
    }
    std::cout << std::format("format {}, duration {} us, bit_rate {}", fmt_ctx_->iformat->name, fmt_ctx_->duration, fmt_ctx_->bit_rate);

    if ((ret = avformat_find_stream_info(fmt_ctx_, nullptr)) < 0) {
      std::cerr << "Could not find stream information:" << av_make_error_string(errbuf, err_char_size, ret) << std::endl;
      return;
    }

    av_dump_format(fmt_ctx_, 0, url.c_str(), 0);
    std::cout << "Successfully opened RTSP stream." << std::endl;
    auto index = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) {
      std::cout << "Could not find video stream"<< std::endl;
      return;
    }
    auto stream_p = fmt_ctx_->streams[index];
    fps = stream_p->avg_frame_rate.num / stream_p->avg_frame_rate.den;
    auto codec_param = stream_p->codecpar;
    const AVCodec* codec_p;
    if ((codec_p = avcodec_find_decoder(codec_param->codec_id))== nullptr){
      std::cout << "Could not find the codec" << std::endl;
      return;
    }
    codec_ctx_ = avcodec_alloc_context3(codec_p);
    if (!codec_ctx_){
      std::cout << "Could not allocated memory for codec context" << std::endl;
      return;
    }

    if (ret = avcodec_parameters_to_context(codec_ctx_, codec_param); ret < 0) {
      std::cout << "Could not copy codec params to codec context:" << av_make_error_string(errbuf, err_char_size, ret);
      return;
    }

    if ((ret = avcodec_open2(codec_ctx_, codec_p, nullptr)) < 0) {
      std::cout << "Could not open codec: " << av_make_error_string(errbuf, err_char_size, ret);
      return;
    }
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
  }
  Streamer::~Streamer() {
    avformat_close_input(&fmt_ctx_);
    avcodec_free_context(&codec_ctx_);
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
  }
  bool Streamer::Valid() {
    return fmt_ctx_ && codec_ctx_;
  }
  AVFrame* Streamer::NextFrame() {
    av_packet_unref(pkt_);
    int ret;
    if ((ret = av_read_frame(fmt_ctx_, pkt_)) < 0) {
      if (ret == AVERROR_EOF) {
        end_ = true;
        return nullptr;
      }
      avs_analyzer::log_error("could not read package", ret);
      return nullptr;
    }
    if ((ret = avcodec_send_packet(codec_ctx_, pkt_)) < 0) {
      avs_analyzer::log_error("Could not send packet to the decoder", ret);
      return nullptr;
    }
    av_frame_unref(frame_);
    ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret == AVERROR_EOF) {
      end_ = true;
      return nullptr;
    } else if (ret < 0) {
      avs_analyzer::log_error("cound not receive frame from the decoder", ret);
      return nullptr;
    }

    return frame_;
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

int runner(Streamer& input_stream,
           const std::function<AVFrame*(AVFrame*, int&)>& func,
           bool free_frame) {

  // --- Configuration ---
  avs_analyzer::FrameRecorder::VideoConfig config{};
  config.width = input_stream.width();
  config.height = input_stream.height();
  config.fps = 25;
  config.bitrate = 20480 * 1000; // 20480 Kbps

  // --- Recorder Setup ---
  avs_analyzer::FrameRecorder recorder("recording.ts", config);
  if (!recorder.initialize()) {
    std::cerr << "Failed to initialize recorder." << std::endl;
    return 1;
  }


  // --- Simulation Loop ---
  int frame_count{};

  while (!input_stream.EoF()) {
    AVFrame* frame;
    if(!(frame = input_stream.NextFrame())) {
      continue;
    }
    frame = func(frame, frame_count);
    recorder.add_video_frame(frame);
    if (free_frame) {
      av_frame_free(&frame);
    }
    // Simulate real-time delay
  }
  // --- Finalization ---
  recorder.finalize();

  std::cout << "Simulation finished." << std::endl;
  return 0;
}