#ifndef ANALYZER_PAINTER_BASE_H
#define ANALYZER_PAINTER_BASE_H

#include <algorithm>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace avs_analyzer {

// Pixel format validation
constexpr bool is_yuv420_format(AVPixelFormat fmt) noexcept {
  return fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P;
}

constexpr bool is_yuva420_format(AVPixelFormat fmt) noexcept {
  return fmt == AV_PIX_FMT_YUVA420P;
}

// Chroma subsampling helpers
struct ChromaInfo {
  unsigned x;
  unsigned y;
  unsigned width;
  unsigned height;
};

inline ChromaInfo get_chroma_info(const AVFrame* frame, int x, int y) noexcept {
  return {
      static_cast<unsigned>(x >> 1),
      static_cast<unsigned>(y >> 1),
      static_cast<unsigned>((frame->width + 1) >> 1),
      static_cast<unsigned>((frame->height + 1) >> 1)
  };
}

inline bool in_bounds(int x, int y, int width, int height) noexcept {
  return static_cast<unsigned>(x) < static_cast<unsigned>(width) &&
         static_cast<unsigned>(y) < static_cast<unsigned>(height);
}

// Bounds checking with clipping
struct BoundingRect {
  int x_start, x_end;
  int y_start, y_end;

  static BoundingRect clip_to_frame(int x, int y, int width, int height,
                                    int frame_width,
                                    int frame_height) noexcept {
    return {
        std::max(0, x),
        std::min(frame_width - 1, x + width - 1),
        std::max(0, y),
        std::min(frame_height - 1, y + height - 1)
    };
  }

  bool is_valid() const noexcept {
    return x_end >= x_start && y_end >= y_start;
  }
};
} // namespace avs_analyzer

#endif  // ANALYZER_PAINTER_BASE_H