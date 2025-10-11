//
// Created by lg on 10/2/25.
//

#ifndef ANALYZER_TEST_HELPER_H
#define ANALYZER_TEST_HELPER_H
#include <functional>
#include <iostream>
#include <thread>  // For std::this_thread::sleep_for

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
class Streamer {
 public:
  explicit Streamer(const std::string& url);
  ~Streamer();
  bool Valid();
  AVFrame* NextFrame();
  [[nodiscard]] bool EoF() const;
  [[nodiscard]] int width() const;
  [[nodiscard]] int height() const;
  [[nodiscard]] AVPixelFormat PixelFormat() const;
  const AVCodecContext* codec() const { return codec_ctx_;}
  int fps{};
  AVRational time_base{};

 private:
  AVFormatContext* fmt_ctx_{};
  AVCodecContext* codec_ctx_{};
  AVPacket* pkt_{};
  AVFrame* frame_{};
  bool end_{};
};
int runner(Streamer&, const std::function<AVFrame*(AVFrame*, int&)>&,
           bool free_frame = true);
#endif  // ANALYZER_TEST_HELPER_H
