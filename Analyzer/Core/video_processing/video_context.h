#ifndef VIDEO_CONTEXT_H
#define VIDEO_CONTEXT_H

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

struct VideoContext {
  AVRational time_base{};
  AVRational sample_aspect_ratio{};
  AVPixelFormat format{AV_PIX_FMT_NONE};
  int width{0};
  int height{0};
  int video_index{-1};
  [[nodiscard]] bool valid() const { return width > 0 && height > 0 && video_index > -1;}
};
#endif // VIDEO_CONTEXT_H