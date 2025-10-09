//
// Created by lg on 9/29/25.
//

#ifndef ANALYZER_FRAME_RECORDER_H
#define ANALYZER_FRAME_RECORDER_H

#include <string>

#include "helper.h"

namespace avs_analyzer {
class Recorder {
 public:
  struct VideoConfig {
    int width;
    int height;
    int fps{25};
  };

  explicit Recorder(VideoConfig config);
  ~Recorder();

  // Initializes the encoder and prepares for recording. Must be called before adding frames.
  bool Initialize(std::string_view file_path);

  // Encodes a single video frame.
  bool AddVideoFrame(AVFrame* frame);

  std::string_view dst() {return file_path_;}

  VideoConfig config;
 private:
  bool OpenOutputSegment();
  void CloseOutputSegment();
  bool EncodeAndWrite(AVFrame* frame);


  AVPacketPtr reused_pkt_;
  std::string file_path_{};

  AVFormatContextPtr output_ctx_{};
  AVCodecContextPtr video_codec_ctx_{};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_FRAME_RECORDER_H
