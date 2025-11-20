#include "painter_optimized.h"
#include "painter_shapes.h"
#include <algorithm>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {
AVFrame* create_blank_yuva420p_optimized(int width, int height) {
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

void BatchPainter::add_filled_rectangle(const cv::Rect& rect,
                                        const YUVColor& color) {
  filled_rectangles_.emplace_back(rect, color);
}

void BatchPainter::add_quadrilateral(const std::vector<cv::Point>& quad_points,
                                     const YUVAColor& color, int thickness) {
  quadrilaterals_.emplace_back(quad_points, color, thickness);
}

void BatchPainter::add_filled_quadrilateral(const std::vector<cv::Point>& quad_points,
                                            const YUVAColor& color) {
  filled_quadrilaterals_.emplace_back(quad_points, color);
}

void BatchPainter::execute() {
  // Process filled quadrilaterals first (backgrounds)
  for (const auto& [quad_points, color] : filled_quadrilaterals_) {
    // Draw filled polygon using existing shape function
    // For now, we'll use the polygon outline function with thick lines
    draw_polygon_yuv420p_optimized(frame_, quad_points, color, 1);
  }

  // Process filled rectangles
  for (const auto& [rect, color] : filled_rectangles_) {
    draw_filled_rectangle_yuv420p_optimized(frame_, rect, color);
  }
  
  // Process quadrilateral outlines
  for (const auto& [quad_points, color, thickness] : quadrilaterals_) {
    draw_polygon_yuv420p_optimized(frame_, quad_points, color, thickness);
  }

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
    draw_rectangle_yuv420p_optimized(frame_, rect, color, thickness);
  }
}

void BatchPainter::clear() {
  rectangles_.clear();
  filled_rectangles_.clear();
  quadrilaterals_.clear();
  filled_quadrilaterals_.clear();
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
      draw_line_yuva420p_optimized(frame,
                                   path[j - 1].x, path[j - 1].y,
                                   path[j].x, path[j].y,
                                   color, thickness, true);
    }

    // Draw end point as a small circle
    if (!path.empty()) {
      const auto& end_point = path.back();
      draw_filled_circle_yuva420p_optimized(frame, end_point.x, end_point.y,
                                            thickness + 1, color, true);
    }
  }
}
} // namespace avs_analyzer