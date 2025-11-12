#ifndef ANALYZER_PAINTER_H
#define ANALYZER_PAINTER_H
#include <string>
#include <vector>

#include "opencv2/core/types.hpp"

extern "C" {
#include "libavutil/frame.h"
}

namespace avs_analyzer {
struct YUVAColor {
  uint8_t y, u, v, a;
};

struct YUVColor {
  uint8_t y, u, v;
};

struct LegendLayout {
  int width;
  int height;
  int pos_x;
  int pos_y;
};

enum class LegendPosition {
  kTopLeft,
  kBottomLeft,
  kTopRight,
  kBottomRight,
};

// scale YUV from full range → limited (for yuv420p)
inline YUVColor scale_for_limited(const YUVColor& c) noexcept {
  auto sy = [](uint8_t v) -> uint8_t {
    return static_cast<uint8_t>(16 + (v * 219 + 127) / 255);
  };
  auto sc = [](uint8_t v) -> uint8_t {
    return static_cast<uint8_t>(16 + (v * 224 + 127) / 255);
  };
  return {sy(c.y), sc(c.u), sc(c.v)};
}

void draw_polygon_yuva420p_thick(AVFrame* f,
                                 const std::vector<cv::Point>& poly,
                                 const YUVAColor& c, int thickness = 2,
                                 bool paint_chroma = true);

void draw_rectangles_yuv420p_fast(AVFrame* f,
                                  const std::vector<cv::Rect>& rects,
                                  const YUVColor& color,
                                  int thickness);

LegendLayout estimate_legend_layout(
    const std::string& font_path,
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::string>& labels_utf8,
    int text_padding,
    int frame_padding,
    int video_w,
    int video_h,
    LegendPosition position_mode = LegendPosition::kTopRight,
    int margin_x = 40,
    int margin_y = 40);

std::pair<int, int> estimate_legend_size(
    const std::string& font_path,
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::string>& labels_utf8,
    int text_padding = 8,
    int frame_padding = 8);

AVFrame* create_blank_yuva420p(int width, int height);

void draw_legend_yuva420p(AVFrame* frame,
                          const std::string& font_path,
                          std::pair<int, int> origin,
                          int font_size,
                          std::pair<int, int> handle_size,
                          int row_spacing,
                          const std::vector<YUVAColor>& colors,
                          const std::vector<std::u8string>& labels,
                          bool write_chroma = true,
                          int text_padding = 8,
                          bool draw_frame = true,
                          int frame_padding = 8,
                          int frame_thickness = 2,
                          YUVAColor frame_color = {210, 128, 128, 255});
}

#endif  // ANALYZER_PAINTER_H