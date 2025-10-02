//
// Created by lg on 9/29/25.
//

#ifndef ANALYZER_FRAME_RECORDER_H
#define ANALYZER_FRAME_RECORDER_H

#include <chrono>
#include <string>
#include <vector>

#include "helper.h"

namespace avs_analyzer {
class FrameRecorder {
 public:
  struct VideoConfig {
    int width;
    int height;
    int fps;
    int bitrate;
  };

  FrameRecorder(std::string_view filename_prefix, VideoConfig config);
  ~FrameRecorder();

  // Initializes the encoder and prepares for recording. Must be called before adding frames.
  bool initialize();

  // Encodes a single video frame.
  bool add_video_frame(AVFrame* frame);

  // Flushes the encoder, merges all segments, and cleans up.
  void finalize();

 private:
  // Internal state and management
  bool open_output_segment();
  void close_output_segment();
  void encode_and_write(AVFrame* frame);

  // Member variables
  VideoConfig m_config;
  std::string file_path_;
  int pts_{0};

  AVFormatContextPtr output_ctx_{};
  AVCodecContextPtr video_codec_ctx_{};
  AVStream* video_stream_{};
  std::chrono::steady_clock::time_point m_segment_start_time;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_FRAME_RECORDER_H
