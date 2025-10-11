//
// Created by lg on 10/12/25.
//

#ifndef ANALYZER_STREAMER_H
#define ANALYZER_STREAMER_H
#include <string>
#include "video_processing/helper.h"
namespace avs_analyzer {
class Streamer {
 public:
  Streamer() = default;
  explicit Streamer(const std::string& url);
  ~Streamer();
  bool Valid();
  bool Init(const std::string& url);
  AVFrame* NextFrame();
  [[nodiscard]] bool EoF() const;
  [[nodiscard]] int width() const;
  [[nodiscard]] int height() const;
  [[nodiscard]] AVPixelFormat PixelFormat() const;
  const AVCodecContext* codec() const { return codec_ctx_.get(); }
  int fps{};
  AVRational time_base{};

 private:
  AVFormatContextPtr fmt_ctx_{};
  AVCodecContextPtr codec_ctx_{};
  AVPacketPtr pkt_{};
  AVFramePtr frame_{};
  bool end_{};
};
}
#endif  // ANALYZER_STREAMER_H
