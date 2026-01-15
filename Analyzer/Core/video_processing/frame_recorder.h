//
// Created by lg on 9/29/25.
//

#ifndef ANALYZER_FRAME_RECORDER_H
#define ANALYZER_FRAME_RECORDER_H

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "helper.h"

namespace avs_analyzer {
class Recorder {
 public:
  enum class OutputType {
    kRtsp,
    kHls,
    kFile,
  };

  enum class HlsSegmentFormat {
    kFmp4,
    kMpegts,
  };

  struct VideoConfig {
    int width;
    int height;
    int fps{25};
  };

  struct HlsConfig {
    HlsSegmentFormat segment_format{HlsSegmentFormat::kFmp4};

    // Duration of each segment in seconds.
    int segment_seconds{5};

    // Sliding window size. 0 means keep all entries.
    int playlist_size{6};

    // If true and playlist_size > 0, old segments are deleted.
    bool delete_segments{true};
  };

  struct HlsPaths {
    std::string playlist;         // full path to .m3u8
    std::string init_filename;    // basename for init file (just filename, not full path)
    std::string segment_pattern;  // full path pattern, e.g. /path/stem_%05d.m4s
    std::string mp4_output;       // suggested mp4 output path
  };

  // Helper to generate consistent HLS-related filenames from a base path.
  // The `base` may be either a path without extension (preferred) or a path
  // already ending in ".m3u8". The returned paths use the provided `hls`
  // configuration to select fmp4 (.m4s + init mp4) vs mpegts (.ts) segments.
  static HlsPaths MakeHlsPaths(std::string_view base);
  static HlsPaths MakeHlsPaths(std::string_view base, const HlsConfig& hls);

  struct CodeConfig {
    int gop_size_scale{2};
    int max_b_frame{2};
    std::string crf{"20"};
    std::string preset{"fast"};
    const char* format_name{nullptr};
    mutable AVDictionaryPtr fmt_options{};
  };

  struct ReencodeConfig {
    std::string preset{"veryslow"};
    int crf{30};
    std::string profile{"high"};
    int gop_size{96};
    int keyint_min{120};
    int sc_threshold{0};
    int max_b_frames{3};
    int refs{4};
    std::string x264_params{
        "cabac=1:aq-mode=2:aq-strength=1.1:deblock=-1,-1:psy-rd=1.0,0.15"};
  };

  explicit Recorder(VideoConfig config);
  ~Recorder();

  // Initializes the encoder and prepares for recording. Must be called before adding frames.
  bool Initialize(std::string_view file_path, OutputType type = OutputType::kFile);
  bool Initialize(std::string_view file_path, OutputType type, const HlsConfig& hls);

  // Encodes a single video frame.
  bool AddVideoFrame(AVFrame* frame);
  bool OpenOutputSegment(const CodeConfig& cconfig);
  void CloseOutputSegment();

  std::string_view dst() {return file_path_;}

  VideoConfig config;
  
  // Returns HLS paths for the last Initialize call if Recorder was initialized
  // in HLS mode, otherwise std::nullopt.
  std::optional<HlsPaths> GetLastHlsPaths() const { return last_hls_paths_; }
  // Returns a suggested MP4 output path for the last initialization.
  // For HLS it returns the derived MP4 path; for file mode it returns the
  // recorder's file path normalized to .mp4; otherwise std::nullopt.
  std::optional<std::string> GetSuggestedMp4() const;

  // Remux the generated HLS (fMP4) playlist/segments into a single MP4 file
  // located at the suggested MP4 path (GetSuggestedMp4()). Returns true on
  // success. Requires the recorder to have been initialized for HLS and the
  // HLS output segments to be present (i.e., after CloseOutputSegment()).
  bool RemuxToSuggestedMp4();

  // Re-encode the HLS output into a smaller MP4 using libx264 settings.
  // This decodes the HLS playlist and encodes to MP4 with the provided config.
  bool ReencodeToSuggestedMp4(const ReencodeConfig& cfg);

 private:
  bool EncodeAndWrite(AVFrame* frame);

  bool initialized_{false};
  int64_t last_pts_{std::numeric_limits<int64_t>::min()};
  AVPacketPtr reused_pkt_;
  std::string file_path_{};

  AVFormatContextPtr output_ctx_{};
  AVCodecContextPtr video_codec_ctx_{};
  AVStream* stream_{};

  // Last initialization mode and derived HLS paths (present only when HLS).
  OutputType last_output_type_{OutputType::kFile};
  std::optional<HlsPaths> last_hls_paths_{};
  HlsConfig last_hls_config_{};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_FRAME_RECORDER_H
