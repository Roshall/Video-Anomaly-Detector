#include "painter.h"
#include "painter_shapes.h"
#include <algorithm>
#include <cstring>

#include "freetype/freetype.h"
#include "painter_text.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {
AVFrame* create_blank_yuva420p(int width, int height) {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    throw std::runtime_error("Failed to allocate AVFrame");
  }

  frame->format = AV_PIX_FMT_YUVA420P;
  frame->width = width;
  frame->height = height;

  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    throw std::runtime_error("av_frame_get_buffer failed");
  }

  // Initialize with neutral values using optimized memset
  // Y plane: neutral gray (128)
  for (int y = 0; y < height; ++y) {
    std::memset(frame->data[0] + y * frame->linesize[0], 128, width);
  }

  // U and V planes: neutral chroma (128)
  const int chroma_width = (width + 1) >> 1;
  const int chroma_height = (height + 1) >> 1;

  for (int y = 0; y < chroma_height; ++y) {
    std::memset(frame->data[1] + y * frame->linesize[1], 128, chroma_width);
    std::memset(frame->data[2] + y * frame->linesize[2], 128, chroma_width);
  }

  // A plane: fully transparent (0)
  for (int y = 0; y < height; ++y) {
    std::memset(frame->data[3] + y * frame->linesize[3], 0, width);
  }

  return frame;
}

BatchPainter::BatchPainter(AVFrame* frame) : frame_(frame) {
  if (!frame_) {
    throw std::invalid_argument("Frame cannot be null");
  }
}

void BatchPainter::add_rectangle(const cv::Rect& rect, const YUVColor& color,
                                 int thickness) {
  rectangles_.emplace_back(rect, color, thickness);
}

void BatchPainter::add_polygon(const std::vector<cv::Point>& points,
                               const YUVColor& color, int thickness) {
  polygons_.emplace_back(points, color, thickness);
}

void BatchPainter::execute() {

  // Group rectangles by color and thickness for better cache efficiency
  std::sort(rectangles_.begin(), rectangles_.end(),
            [](const auto& a, const auto& b) {
              const auto& [rect_a, color_a, thickness_a] = a;
              const auto& [rect_b, color_b, thickness_b] = b;

              if (thickness_a != thickness_b) return thickness_a < thickness_b;
              if (color_a.y != color_b.y) return color_a.y < color_b.y;
              if (color_a.u != color_b.u) return color_a.u < color_b.u;
              return color_a.v < color_b.v;
            });

  // Process rectangles
  for (const auto& [rect, color, thickness] : rectangles_) {
    draw_rectangle_yuv420p(frame_, rect, color, thickness);
  }

  // Process polygon outlines (YUV, no alpha)
  for (const auto& [points, color, thickness] : polygons_) {
    draw_polygon_yuv420p(frame_, points, color, thickness);
  }
}

void BatchPainter::clear() {
  rectangles_.clear();
  polygons_.clear();
}

void draw_detection_boxes(AVFrame* frame,
                          const std::vector<cv::Rect>& boxes,
                          const std::vector<YUVColor>& colors,
                          int thickness) {
  if (!frame || boxes.empty()) return;

  const size_t num_boxes = std::min(boxes.size(), colors.size());

  BatchPainter painter(frame);

  for (size_t i = 0; i < num_boxes; ++i) {
    painter.add_rectangle(boxes[i], colors[i], thickness);
  }

  painter.execute();
}

void draw_tracking_paths(AVFrame* frame,
                         const std::vector<std::vector<cv::Point>>& paths,
                         const std::vector<YUVAColor>& colors,
                         int thickness) {
  if (!frame || paths.empty() || !is_yuva420_format(
          static_cast<AVPixelFormat>(frame->format))) {
    return;
  }

  const size_t num_paths = std::min(paths.size(), colors.size());

  for (size_t i = 0; i < num_paths; ++i) {
    const auto& path = paths[i];
    const auto& color = colors[i];

    if (path.size() < 2) continue;

    // Draw lines between consecutive points
    for (size_t j = 1; j < path.size(); ++j) {
      draw_line_yuva420p(frame,
                                   path[j - 1].x, path[j - 1].y,
                                   path[j].x, path[j].y,
                                   color, thickness, true);
    }

    // Draw end point as a small circle
    if (!path.empty()) {
      const auto& end_point = path.back();
      draw_filled_circle_yuva420p(frame, end_point.x, end_point.y,
                                            thickness + 1, color, true);
    }
  }
}
// Estimate legend size (simplified version without FreeType dependency)
std::pair<int, int> estimate_legend_size(
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::u8string>& labels_utf8,
    int text_padding,
    int frame_padding) {
  // Simplified estimation without FreeType
  const auto [handle_w, handle_h] = handle_wh;
  const int row_h = std::max(handle_h, font_px);
  const int item_count = static_cast<int>(labels_utf8.size());

  // Estimate text width (rough approximation)
  int max_text_w = 0;
  for (const auto& label : labels_utf8) {
    max_text_w = std::max(max_text_w,
                          static_cast<int>(label.length() * font_px * 0.6));
  }

  const int content_h = item_count > 0
                          ? row_h * item_count + row_spacing * (item_count - 1)
                          : 0;
  const int content_w = handle_w + text_padding + max_text_w;

  const int total_w = content_w + frame_padding * 2;
  const int total_h = content_h + frame_padding * 2;

  return {total_w, total_h};
}

// Accurate legend size estimation using FreeType
std::pair<int, int> estimate_legend_size_accurate(
    const std::string& font_path,
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::u8string>& labels_utf8,
    int text_padding,
    int frame_padding) {
  // Keep existing font-path based implementation for backward compatibility
  try {
    FT_Library ft_library;
    if (FT_Init_FreeType(&ft_library) != 0) {
      return estimate_legend_size(font_px, handle_wh, row_spacing,
                                  labels_utf8, text_padding, frame_padding);
    }

    FT_Face face;
    if (FT_New_Face(ft_library, font_path.c_str(), 0, &face) != 0) {
      FT_Done_FreeType(ft_library);
      return estimate_legend_size(font_px, handle_wh, row_spacing,
                                  labels_utf8, text_padding, frame_padding);
    }

    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(font_px)) != 0) {
      FT_Done_Face(face);
      FT_Done_FreeType(ft_library);
      return estimate_legend_size(font_px, handle_wh, row_spacing,
                                  labels_utf8, text_padding, frame_padding);
    }

    // Measure actual text widths using FreeType
    int max_text_w = 0;
    for (const auto& label : labels_utf8) {
      int text_width = 0;

      // Use UTF-8 aware measurement in painter_text if available
      {
        // Convert to string for utf8 helper
        std::string s(reinterpret_cast<const char*>(label.c_str()));
        // Use simple char-based fallback (still better than naive length)
        for (const auto c : s) {
          if (FT_Load_Char(face, c, FT_LOAD_DEFAULT) == 0) {
            text_width += static_cast<int>(face->glyph->advance.x >> 6);
          }
        }
      }
       max_text_w = std::max(max_text_w, text_width);
    }

    FT_Done_Face(face);
    FT_Done_FreeType(ft_library);

    const auto [handle_w, handle_h] = handle_wh;
    const int row_h = std::max(handle_h, font_px);
    const int item_count = static_cast<int>(labels_utf8.size());

    const int content_h = item_count > 0
                            ? row_h * item_count + row_spacing * (
                                item_count - 1)
                            : 0;
    const int content_w = handle_w + text_padding + max_text_w;

    const int total_w = content_w + frame_padding * 2;
    // we offset text a little bit. add font_px to ensure the text will not out
    // of boundary
    const int total_h = content_h + frame_padding * 2 + font_px;

    return {total_w, total_h};
  } catch (...) {
    // Fallback on any exception
    return estimate_legend_size(font_px, handle_wh, row_spacing,
                                labels_utf8, text_padding, frame_padding);
  }
}
// Overload: measure using an already-loaded FreeType face (UTF-8 aware)
std::pair<int, int> estimate_legend_size_accurate(
    FT_Face face,
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::u8string>& labels_utf8,
    int text_padding,
    int frame_padding) {
  if (!face) return estimate_legend_size(font_px, handle_wh, row_spacing,
                                         labels_utf8, text_padding, frame_padding);

  int max_text_w = 0;
  for (const auto& label : labels_utf8) {
    // UTF-8 aware measurement
    max_text_w = std::max(max_text_w, utf8::measure_text_width(face, std::string(reinterpret_cast<const char*>(label.c_str()))));
  }

  const auto [handle_w, handle_h] = handle_wh;
  const int row_h = std::max(handle_h, font_px);
  const int item_count = static_cast<int>(labels_utf8.size());

  const int content_h = item_count > 0
                          ? row_h * item_count + row_spacing * (item_count - 1)
                          : 0;
  const int content_w = handle_w + text_padding + max_text_w;

  const int total_w = content_w + frame_padding * 2;
  const int total_h = content_h + frame_padding * 2 + font_px;
  return {total_w, total_h};
}


// Legend layout estimation
LegendLayout estimate_legend_layout(
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::u8string>& labels_utf8,
    int text_padding,
    int frame_padding,
    int video_w,
    int video_h,
    const LegendPosition position_mode,
    int margin_x,
    int margin_y) {
  const auto [total_w, total_h] = estimate_legend_size(
      font_px, handle_wh, row_spacing, labels_utf8,
      text_padding, frame_padding);

  int pos_x = margin_x;
  int pos_y = margin_y;

  switch (position_mode) {
    case LegendPosition::kTopRight:
      pos_x = std::max(0, video_w - total_w - margin_x);
      pos_y = margin_y;
      break;
    case LegendPosition::kBottomRight:
      pos_x = std::max(0, video_w - total_w - margin_x);
      pos_y = std::max(0, video_h - total_h - margin_y);
      break;
    case LegendPosition::kTopLeft:
      pos_x = margin_x;
      pos_y = margin_y;
      break;
    case LegendPosition::kBottomLeft:
      pos_x = margin_x;
      pos_y = std::max(0, video_h - total_h - margin_y);
      break;
  }

  return {total_w, total_h, pos_x, pos_y};
}

} // namespace avs_analyzer