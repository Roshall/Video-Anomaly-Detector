//
// Created by lg on 10/7/25.
//

#ifndef ANALYZER_FORMAT_CONVERTOR_H
#define ANALYZER_FORMAT_CONVERTOR_H
#include <libavutil/pixfmt.h>

#include <opencv2/opencv.hpp>

#include "helper.h"

struct SwsContext;

namespace avs_analyzer {
struct AVBufferDeleter {
  void operator()(uint8_t * ptr) const {
    av_free(ptr);
  }
};

using AVBufferPtr = std::unique_ptr<uint8_t, AVBufferDeleter>;
class FormatConvertor {
 public:
  FormatConvertor(int in_width, int in_height, AVPixelFormat in_fmt, int out_width, int out_height);
  FormatConvertor(FormatConvertor& other) = delete;
  FormatConvertor(FormatConvertor&& other) noexcept ;
  cv::Mat operator() (AVFrame* original_frame);
//  AVFramePtr CopyCurrentFrame();
 private:
  int width_;
  int height_;
  int in_height_;
  AVFramePtr bgr_frame_;
  AVBufferPtr bgr_buffer_;
  SwsContextPtr sws_context_;
};
}

#endif  // ANALYZER_FORMAT_CONVERTOR_H
