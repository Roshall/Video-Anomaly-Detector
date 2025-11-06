//
// Created by lg on 10/7/25.
//

#include "format_convertor.h"

#include <stdexcept>

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {
FormatConvertor::FormatConvertor(int in_width, int in_height,
                                               AVPixelFormat in_fmt,
                                               int out_width, int out_height)
    : width_{out_width},
      height_{out_height},
      in_height_{in_height},
      bgr_frame_{av_frame_alloc()},
      sws_context_{sws_getContext(in_width, in_height, in_fmt, out_width,
                                  out_height, AV_PIX_FMT_BGR24, SWS_BILINEAR,
                                  nullptr, nullptr, nullptr)} {
  
  if (!bgr_frame_) {
    throw std::runtime_error("av_frame_alloc() failed");
  }
  if (!sws_context_) {
    throw std::runtime_error("sws_getContext() failed");
  }
  const auto buff_size =
      av_image_get_buffer_size(AV_PIX_FMT_BGR24, out_width, out_height, 1);
  bgr_buffer_.reset(static_cast<uint8_t*>(av_malloc(buff_size)));
  if (!bgr_buffer_) {
    throw std::runtime_error("av_malloc() failed for bgr buffer");
  }
  av_image_fill_arrays(bgr_frame_->data, bgr_frame_->linesize, bgr_buffer_.get(),
                       AV_PIX_FMT_BGR24, out_width, out_height, 1);
}
FormatConvertor::FormatConvertor(FormatConvertor&& other) noexcept
    : width_{other.width_},
      height_{other.height_},
      in_height_{other.in_height_},
      bgr_frame_(std::move(other.bgr_frame_)),
      bgr_buffer_(std::move(other.bgr_buffer_)),
      sws_context_{std::move(other.sws_context_)} {}

cv::Mat FormatConvertor::operator()(const AVFrame* original_frame) {
  if (!original_frame) {
    throw std::invalid_argument("original_frame is null");
  }
  sws_scale(sws_context_.get(), original_frame->data, original_frame->linesize,
            0, in_height_, bgr_frame_->data, bgr_frame_->linesize);

  return {height_, width_, CV_8UC3, bgr_frame_->data[0]};
}
}