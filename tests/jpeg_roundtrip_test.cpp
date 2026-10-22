#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "jpeg_encoder.h"

using namespace avs_analyzer;

namespace {

AVFramePtr MakeYuv420pFrame(int width, int height, bool gradient) {
  AVFramePtr frame{av_frame_alloc()};
  if (!frame) return nullptr;
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = width;
  frame->height = height;
  if (av_frame_get_buffer(frame.get(), 32) < 0) return nullptr;
  if (av_frame_make_writable(frame.get()) < 0) return nullptr;

  for (int y = 0; y < height; ++y) {
    uint8_t* row = frame->data[0] + y * frame->linesize[0];
    for (int x = 0; x < width; ++x) {
      row[x] = gradient
                   ? static_cast<uint8_t>((x * 4 + y * 3) & 0xFF)
                   : static_cast<uint8_t>(128);
    }
  }
  for (int y = 0; y < height / 2; ++y) {
    std::memset(frame->data[1] + y * frame->linesize[1], 128, width / 2);
    std::memset(frame->data[2] + y * frame->linesize[2], 128, width / 2);
  }
  frame->pts = 7;
  frame->best_effort_timestamp = 7;
  return frame;
}

double MeanAbsoluteErrorY(const AVFrame& a, const AVFrame& b) {
  double sum = 0.0;
  int count = 0;
  for (int y = 0; y < a.height; ++y) {
    const uint8_t* ra = a.data[0] + y * a.linesize[0];
    const uint8_t* rb = b.data[0] + y * b.linesize[0];
    for (int x = 0; x < a.width; ++x) {
      sum += std::abs(static_cast<int>(ra[x]) - static_cast<int>(rb[x]));
      ++count;
    }
  }
  return count > 0 ? sum / count : 0.0;
}

}  // namespace

TEST(JpegRoundTripTest, EncodeDecodePreservesDimensionsFormatAndContent) {
  constexpr int kWidth = 64;
  constexpr int kHeight = 64;
  AVFramePtr source = MakeYuv420pFrame(kWidth, kHeight, true);
  ASSERT_TRUE(source);

  JPEGEncoder encoder(kWidth, kHeight, 5);
  const std::vector<uint8_t> jpeg = encoder.EncodeToBuffer(source.get());
  ASSERT_FALSE(jpeg.empty());
  // A valid JPEG starts with SOI and ends with EOI markers.
  ASSERT_GE(jpeg.size(), 4u);
  EXPECT_EQ(jpeg[0], 0xFF);
  EXPECT_EQ(jpeg[1], 0xD8);
  EXPECT_EQ(jpeg[jpeg.size() - 2], 0xFF);
  EXPECT_EQ(jpeg[jpeg.size() - 1], 0xD9);

  AVFramePtr decoded = encoder.DecodeToFrame(jpeg);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->width, kWidth);
  EXPECT_EQ(decoded->height, kHeight);
  EXPECT_EQ(decoded->format, AV_PIX_FMT_YUV420P);
  EXPECT_LT(MeanAbsoluteErrorY(*source, *decoded), 8.0);
}

TEST(JpegRoundTripTest, DecodeRejectsInvalidData) {
  JPEGEncoder encoder(64, 64, 5);
  const std::vector<uint8_t> garbage{0x01, 0x02, 0x03, 0x04};
  EXPECT_FALSE(encoder.DecodeToFrame(garbage));
  EXPECT_FALSE(encoder.DecodeToFrame({}));
}

TEST(JpegRoundTripTest, EncoderHandlesSizeChanges) {
  AVFramePtr small = MakeYuv420pFrame(32, 32, false);
  AVFramePtr large = MakeYuv420pFrame(64, 64, false);
  ASSERT_TRUE(small);
  ASSERT_TRUE(large);

  JPEGEncoder encoder(32, 32, 5);
  const auto small_jpeg = encoder.EncodeToBuffer(small.get());
  ASSERT_FALSE(small_jpeg.empty());

  const auto large_jpeg = encoder.EncodeToBuffer(large.get());
  ASSERT_FALSE(large_jpeg.empty());

  AVFramePtr decoded = encoder.DecodeToFrame(large_jpeg);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->width, 64);
  EXPECT_EQ(decoded->height, 64);
  EXPECT_EQ(decoded->format, AV_PIX_FMT_YUV420P);
}
