extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>    
}

struct VideoContext {
  AVRational time_base;
  AVRational sample_aspect_ratio;
  AVPixelFormat format;
  int width{0};
  int height{0};
  int video_index{-1};
  bool valid() const { return width > 0 && height > 0 && video_index > -1;}
};