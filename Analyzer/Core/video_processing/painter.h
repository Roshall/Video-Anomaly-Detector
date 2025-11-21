#ifndef ANALYZER_PAINTER_OPTIMIZED_H
#define ANALYZER_PAINTER_OPTIMIZED_H

#include "painter_base.h"
#include "painter_shapes.h"
#include <vector>

namespace avs_analyzer {

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

LegendLayout estimate_legend_layout(
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::u8string>& labels_utf8,
    int text_padding,
    int frame_padding,
    int video_w,
    int video_h,
    LegendPosition position_mode = LegendPosition::kTopRight,
    int margin_x = 40,
    int margin_y = 40);

std::pair<int, int> estimate_legend_size(
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::u8string>& labels_utf8,
    int text_padding = 8,
    int frame_padding = 8);

// Accurate legend size estimation using FreeType
std::pair<int, int> estimate_legend_size_accurate(
    const std::string& font_path,
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::u8string>& labels_utf8,
    int text_padding = 8,
    int frame_padding = 8);
// Factory function to create optimized frame
AVFrame* create_blank_yuva420p(int width, int height);

// Optimized batch operations
class BatchPainter {
public:
  explicit BatchPainter(AVFrame* frame);

  // Batch rectangle drawing
  void add_rectangle(const cv::Rect& rect, const YUVColor& color,
                     int thickness);
  
  // Batch polygon (outline) drawing with YUV (no alpha), used to directly draw ROIs on YUV420P
  void add_polygon(const std::vector<cv::Point>& points, const YUVColor& color, int thickness);

  // Execute all batched operations
  void execute();
  void clear();

private:
  AVFrame* frame_;
  std::vector<std::tuple<cv::Rect, YUVColor, int>> rectangles_;
  std::vector<std::tuple<std::vector<cv::Point>, YUVColor, int>> polygons_;
};

// High-level convenience functions
void draw_detection_boxes(AVFrame* frame,
                          const std::vector<cv::Rect>& boxes,
                          const std::vector<YUVColor>& colors,
                          int thickness = 2);

void draw_tracking_paths(AVFrame* frame,
                         const std::vector<std::vector<cv::Point>>& paths,
                         const std::vector<YUVAColor>& colors,
                         int thickness = 2);
} // namespace avs_analyzer

#endif  // ANALYZER_PAINTER_OPTIMIZED_H