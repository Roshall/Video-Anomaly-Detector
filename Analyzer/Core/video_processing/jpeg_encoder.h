//
// Created by lg on 10/10/25.
//

#ifndef ANALYZER_JPEG_ENCODER_H
#define ANALYZER_JPEG_ENCODER_H
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <stdexcept>
#include <string>
#include <cstdio>
#include <cerrno>
#include <fstream>
#include <mutex>

#include <helper.h>

namespace avs_analyzer {

class JPEGEncoder {
 private:
  AVCodecContextPtr ctx = nullptr;
  SwsContextPtr sws_ctx = nullptr;

  int current_width = 0;
  int current_height = 0;
  AVPixelFormat current_src_fmt = AV_PIX_FMT_NONE;

  // Quality controls
  int bit_rate = 4'000'000;     // optional
  int quality_q = -1;           // 2..31 (lower = better). If <0, use bitrate.

  AVFramePtr conv_frame = nullptr; // cached conversion frame when needed
  int64_t pts_ = 0;
  std::recursive_mutex mutex_;

  void ensureConvFrame(int w, int h, AVPixelFormat fmt);

  void initCodec(int width, int height);

 public:
  JPEGEncoder(int initial_width, int initial_height, int initial_quality_q = -1);

  void setBitRate(int new_bit_rate);

  void setQualityQ(int q /*2..31, lower is better*/);

  std::pair<int, int> saveFrame(AVFrame *in, const std::string &filename);
};
}
#endif  // ANALYZER_JPEG_ENCODER_H
