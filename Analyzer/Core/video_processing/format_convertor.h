//
// Created by lg on 10/7/25.
//

#ifndef ANALYZER_FORMAT_CONVERTOR_H
#define ANALYZER_FORMAT_CONVERTOR_H
extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
}

#include <opencv2/opencv.hpp>

#include "helper.h"

struct SwsContext;

namespace avs_analyzer {

class FormatConvertor {
 public:
  FormatConvertor(int in_width, int in_height, AVPixelFormat in_fmt, int out_width, int out_height);
  FormatConvertor(FormatConvertor& other) = delete;
  cv::Mat operator() (const AVFrame* original_frame);
  FormatConvertor(FormatConvertor&& other) noexcept ;

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
