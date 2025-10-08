//
// Created by lg on 10/7/25.
//

#include "format_convertor.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {
FormatConvertor::FormatConvertor(int in_width, int in_height,
                                               AVPixelFormat in_fmt,
                                               int out_width, int out_height)
    : width_{out_width},
      height_{out_width},
      bgr_frame_{av_frame_alloc()},
      sws_context_{sws_getContext(in_width, in_height, in_fmt, out_width,
                                  out_height, AV_PIX_FMT_BGR24, SWS_BICUBIC,
                                  nullptr, nullptr, nullptr)} {
  int buff_size =
      av_image_get_buffer_size(AV_PIX_FMT_BGR24, out_width, out_height, 1);
  bgr_buffer_.reset((uint8_t*)av_malloc(buff_size));
  av_image_fill_arrays(bgr_frame_->data, bgr_frame_->linesize, bgr_buffer_.get(),
                       AV_PIX_FMT_BGR24, out_width, out_height, 1);
}
FormatConvertor::FormatConvertor(FormatConvertor&& other) noexcept
    : width_{other.width_},
      height_{other.height_},
      bgr_frame_(std::move(other.bgr_frame_)),
      sws_context_{std::move(other.sws_context_)} {}

cv::Mat FormatConvertor::operator()(
    AVFrame* original_frame) const {
  sws_scale(sws_context_.get(), original_frame->data, original_frame->linesize,
            0, height_, bgr_frame_->data, bgr_frame_->linesize);

  return {height_, width_, CV_8UC3, bgr_frame_->data[0]};
}
}