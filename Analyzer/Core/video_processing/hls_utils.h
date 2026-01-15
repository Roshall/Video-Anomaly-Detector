// Created by refactor tool
#ifndef ANALYZER_HLS_UTILS_H
#define ANALYZER_HLS_UTILS_H

#include "frame_recorder.h"
#include <string>

namespace avs_analyzer {

struct ReencodeConfig {
  std::string preset{"veryslow"};
  int crf{30};
  std::string profile{"high"};
  int gop_size{96};
  int keyint_min{120};
  int sc_threshold{0};
  int max_b_frames{3};
  int refs{4};
  std::string x264_params{"cabac=1:aq-mode=2:aq-strength=1.1:deblock=-1,-1:psy-rd=1.0,0.15"};
};

// Remux the HLS (fMP4) playlist/segments into a single MP4 file at the
// path derived from `paths.mp4_output`.
bool RemuxHlsToSuggestedMp4(const Recorder::HlsPaths& paths);

// Re-encode the HLS output into a smaller MP4 using libx264 settings.
bool ReencodeHlsToSuggestedMp4(const Recorder::HlsPaths& paths, const ReencodeConfig& cfg);

// Remove HLS playlist, init file and segment files derived from `paths`.
bool RemoveHlsFiles(const Recorder::HlsPaths& paths);

}  // namespace avs_analyzer

#endif  // ANALYZER_HLS_UTILS_H
