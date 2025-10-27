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
  [[nodiscard]] bool Valid() const;
  bool Init(const std::string& url);
  AVFrame* NextFrame();
  [[nodiscard]] bool EoF() const;
  [[nodiscard]] int width() const;
  [[nodiscard]] int height() const;
  [[nodiscard]] AVPixelFormat PixelFormat() const;
  [[nodiscard]] const AVCodecContext* codec() const { return codec_ctx_.get(); }
  [[nodiscard]] const AVFormatContext* format_context() const {return fmt_ctx_.get();}
  [[nodiscard]] float GetFps() const;
  [[nodiscard]] const AVStream* stream() const { return stream_; }
  AVRational time_base{};

 private:
  AVFormatContextPtr fmt_ctx_{};
  AVCodecContextPtr codec_ctx_{};
  AVStream* stream_{};
  AVPacketPtr pkt_{};
  AVFramePtr frame_{};
  bool end_{};
};
}
#endif  // ANALYZER_STREAMER_H
