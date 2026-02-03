#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
}

#include "frame_recorder.h"
#include "hls_utils.h"

namespace {

std::filesystem::path MakeTempDir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto dir = base / std::filesystem::path(
                          "bxc_videoanalyzer_hls_" +
                          std::to_string(
                              std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count()));
  std::filesystem::create_directories(dir);
  return dir;
}

AVFrame* MakeYuv420pFrame(int width, int height, int64_t best_effort_ts,
                          AVRational time_base) {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return nullptr;
  }

  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = width;
  frame->height = height;
  frame->time_base = time_base;
  frame->best_effort_timestamp = best_effort_ts;

  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    return nullptr;
  }
  if (av_frame_make_writable(frame) < 0) {
    av_frame_free(&frame);
    return nullptr;
  }

  const uint8_t y = static_cast<uint8_t>(best_effort_ts % 255);
  for (int r = 0; r < height; ++r) {
    std::memset(frame->data[0] + r * frame->linesize[0], y, width);
  }
  for (int r = 0; r < height / 2; ++r) {
    std::memset(frame->data[1] + r * frame->linesize[1], 128, width / 2);
    std::memset(frame->data[2] + r * frame->linesize[2], 128, width / 2);
  }

  frame->pict_type = AV_PICTURE_TYPE_NONE;
  return frame;
}

std::string ReadAllText(const std::filesystem::path& p) {
  std::ifstream in(p);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST(FrameRecorderHlsTest, WritesFmp4PlaylistAndSegments) {
  using Recorder = avs_analyzer::Recorder;

  const auto out_dir = MakeTempDir();
  const auto playlist = out_dir / "out.m3u8";

  Recorder::VideoConfig config{};
  config.width = 64;
  config.height = 64;
  config.fps = 10;

  Recorder recorder(config);

  Recorder::HlsConfig hls{};
  hls.segment_format = Recorder::HlsSegmentFormat::kFmp4;
  hls.segment_seconds = 1;
  hls.playlist_size = 6;
  hls.delete_segments = false;  // keep files for assertions

  ASSERT_TRUE(recorder.Initialize(playlist.string(), Recorder::OutputType::kHls, hls));
  EXPECT_EQ(recorder.dst(), playlist.string());

  // Instance getter should match
  {
    auto last = recorder.last_hls_paths;
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->playlist, playlist.string());
    EXPECT_EQ(last->init_filename, "out_init.mp4");
    EXPECT_EQ(last->segment_pattern, (out_dir / "out_%05d.m4s").string());
    EXPECT_EQ(last->mp4_output, (out_dir / "out.mp4").string());
  }

  // Also check helper output for the same base path (without extension)
  {
    const auto base = (out_dir / "out").string();
    const auto paths = Recorder::MakeHlsPaths(base, hls);
    EXPECT_EQ(paths.playlist, (out_dir / "out.m3u8").string());
    EXPECT_EQ(paths.init_filename, "out_init.mp4");
    EXPECT_EQ(paths.segment_pattern, (out_dir / "out_%05d.m4s").string());
    EXPECT_EQ(paths.mp4_output, (out_dir / "out.mp4").string());
  }

  // Suggested mp4 should match for HLS
  {
    auto s = recorder.GetSuggestedMp4();
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(*s, (out_dir / "out.mp4").string());
  }

  // Write frames and finalize the playlist + segments
  {
    const AVRational tb{1, config.fps};
    for (int i = 0; i < 30; ++i) {
      AVFrame* frame = MakeYuv420pFrame(config.width, config.height, i, tb);
      ASSERT_NE(frame, nullptr);
      ASSERT_TRUE(recorder.AddVideoFrame(frame));
      av_frame_free(&frame);
    }
    recorder.CloseOutputSegment();

    // playlist and segments should exist
    ASSERT_TRUE(std::filesystem::exists(playlist));
    const std::string content = ReadAllText(playlist);
    ASSERT_NE(content.find("#EXTM3U"), std::string::npos);

    const auto init_file = out_dir / "out_init.mp4";
    EXPECT_TRUE(std::filesystem::exists(init_file));

    std::vector<std::filesystem::path> segments;
    {
      std::ifstream in(playlist);
      std::string line;
      while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
          continue;
        }
        segments.emplace_back(out_dir / line);
      }
    }

    ASSERT_FALSE(segments.empty());
    for (const auto& seg : segments) {
      EXPECT_TRUE(std::filesystem::exists(seg)) << seg.string();
      if (std::filesystem::exists(seg)) {
        EXPECT_GT(std::filesystem::file_size(seg), 0u) << seg.string();
      }
    }

    // Remux into single mp4 and validate
    ASSERT_TRUE(avs_analyzer::RemuxHlsToSuggestedMp4(*recorder.last_hls_paths));
    auto mp4 = recorder.GetSuggestedMp4();
    ASSERT_TRUE(mp4.has_value());

    AVFormatContext* fmt = nullptr;
    ASSERT_EQ(avformat_open_input(&fmt, mp4->c_str(), nullptr, nullptr), 0);
    avs_analyzer::AVFormatContextPtr fmtp{fmt};
    EXPECT_GT(fmtp->nb_streams, 0);

    // Cleanup HLS files
    ASSERT_TRUE(avs_analyzer::RemoveHlsFiles(*recorder.last_hls_paths));
    EXPECT_FALSE(std::filesystem::exists(playlist));
    EXPECT_FALSE(std::filesystem::exists(out_dir / "out_init.mp4"));

    // Segments should be removed
    for (auto& e : std::filesystem::directory_iterator(out_dir)) {
      const auto fn = e.path().filename().string();
      EXPECT_FALSE(fn.rfind("out_", 0) == 0 && fn.find(".m4s") != std::string::npos);
    }
  }
}

TEST(FrameRecorderSuggestedMp4Test, FileModeSuggestion) {
  using Recorder = avs_analyzer::Recorder;
  Recorder::VideoConfig config{};
  config.width = 32;
  config.height = 32;
  config.fps = 10;

  Recorder recorder(config);
  // initialize file mode with a .ts path and expect suggested .mp4
  ASSERT_TRUE(recorder.Initialize("/tmp/foo.ts", Recorder::OutputType::kFile));
  auto s = recorder.GetSuggestedMp4();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(*s, std::string("/tmp/foo.mp4"));

}

TEST(FrameRecorderHlsCleanupTest, RemovesExpectedFilesAndDir) {
  using Recorder = avs_analyzer::Recorder;
  Recorder::VideoConfig cfg{};
  cfg.width = 16;
  cfg.height = 16;
  cfg.fps = 10;

  Recorder recorder(cfg);

  auto dir = std::filesystem::temp_directory_path() / std::filesystem::path("hls_cleanup_test");
  if (std::filesystem::exists(dir)) std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  auto playlist = dir / "test.m3u8";
  auto init = dir / "test_init.mp4";
  auto seg1 = dir / "test_00001.m4s";
  auto seg2 = dir / "test_00002.m4s";

  // create files
  std::ofstream(playlist.string()).put('x');
  std::ofstream(init.string()).put('x');
  std::ofstream(seg1.string()).put('x');
  std::ofstream(seg2.string()).put('x');

  Recorder::HlsPaths paths;
  paths.playlist = playlist.string();
  paths.init_filename = init.filename().string();
  paths.segment_pattern = (dir / std::string("test_%05d.m4s")).string();
  paths.mp4_output = (dir / std::string("test.mp4")).string();

  recorder.last_output_type = Recorder::OutputType::kHls;
  recorder.last_hls_paths = std::move(paths);

  ASSERT_TRUE(std::filesystem::exists(playlist));
  ASSERT_TRUE(std::filesystem::exists(init));
  ASSERT_TRUE(std::filesystem::exists(seg1));
  ASSERT_TRUE(std::filesystem::exists(seg2));

  ASSERT_TRUE(avs_analyzer::RemoveHlsFiles(*recorder.last_hls_paths));

  EXPECT_FALSE(std::filesystem::exists(playlist));
  EXPECT_FALSE(std::filesystem::exists(init));
  EXPECT_FALSE(std::filesystem::exists(seg1));
  EXPECT_FALSE(std::filesystem::exists(seg2));

  // Directory should be removed when empty
  EXPECT_FALSE(std::filesystem::exists(dir));
}
