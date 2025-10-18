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

  struct CodeConfig {
    int gop_size_scale{2};
    int max_b_frame{2};
    std::string crf{"20"};
    std::string preset{"fast"};
    const char* format_name{nullptr};
    mutable AVDictionaryPtr fmt_options{};
  };

  explicit Recorder(VideoConfig config);
  ~Recorder();

  // Initializes the encoder and prepares for recording. Must be called before adding frames.
  bool Initialize(std::string_view file_path, bool file=true);

  // Encodes a single video frame.
  bool AddVideoFrame(AVFrame* frame);
  bool OpenOutputSegment(const CodeConfig& cconfig);
  void CloseOutputSegment();

  std::string_view dst() {return file_path_;}

  VideoConfig config;
 private:
  bool EncodeAndWrite(AVFrame* frame);

  bool initialized_{false};
  AVPacketPtr reused_pkt_;
  std::string file_path_{};

  AVFormatContextPtr output_ctx_{};
  AVCodecContextPtr video_codec_ctx_{};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_FRAME_RECORDER_H
