#include "painter_shapes.h"
#include "painter_primitives.h"
#include <algorithm>
#include <cmath>

namespace avs_analyzer {
void draw_rectangle_yuv420p_optimized(AVFrame* frame, const cv::Rect& rect,
                                      const YUVColor& color, int thickness) {
  if (thickness <= 0 || !frame) return;

  const auto fmt = static_cast<AVPixelFormat>(frame->format);
  if (!is_yuv420_format(fmt)) return;

  const auto bounds = BoundingRect::clip_to_frame(
      rect.x, rect.y, rect.width, rect.height,
      frame->width, frame->height);

  if (!bounds.is_valid()) return;

  // If thickness is too large, fill the entire rectangle
  if (thickness >= std::min(rect.width, rect.height) / 2) {
    draw_filled_rectangle_yuv420p_optimized(frame, rect, color);
    return;
  }

  // Draw four edges efficiently
  // Top edge
  draw_horizontal_line_yuv420p_optimized(frame, bounds.x_start, bounds.x_end,
                                         bounds.y_start, color, thickness);

  // Bottom edge
  const int bottom_y = bounds.y_end - thickness + 1;
  if (bottom_y > bounds.y_start + thickness) {
    draw_horizontal_line_yuv420p_optimized(frame, bounds.x_start, bounds.x_end,
                                           bottom_y, color, thickness);
  }

  // Left edge (excluding corners)
  const int left_start_y = bounds.y_start + thickness;
  const int left_end_y = bounds.y_end - thickness;
  if (left_end_y >= left_start_y) {
    draw_vertical_line_yuv420p_optimized(frame, bounds.x_start,
                                         left_start_y, left_end_y,
                                         color, thickness);
  }

  // Right edge (excluding corners)
  const int right_start_y = bounds.y_start + thickness;
  const int right_end_y = bounds.y_end - thickness;
  const int right_x = bounds.x_end - thickness + 1;
  if (right_end_y >= right_start_y && right_x <= bounds.x_end) {
    draw_vertical_line_yuv420p_optimized(frame, right_x,
                                         right_start_y, right_end_y,
                                         color, thickness);
  }
}

void draw_filled_rectangle_yuv420p_optimized(AVFrame* frame,
                                             const cv::Rect& rect,
                                             const YUVColor& color) {
  if (!frame) return;

  const auto fmt = static_cast<AVPixelFormat>(frame->format);
  if (!is_yuv420_format(fmt)) return;

  const auto bounds = BoundingRect::clip_to_frame(
      rect.x, rect.y, rect.width, rect.height,
      frame->width, frame->height);

  if (!bounds.is_valid()) return;

  // Fill Y plane
  for (int y = bounds.y_start; y <= bounds.y_end; ++y) {
    uint8_t* y_row = frame->data[0] + y * frame->linesize[0];
    fill_horizontal_line_fast(y_row, bounds.x_start, bounds.x_end, color.y);
  }

  // Fill chroma planes (subsampled)
  const int chroma_x_start = bounds.x_start >> 1;
  const int chroma_x_end = bounds.x_end >> 1;
  const int chroma_y_start = bounds.y_start >> 1;
  const int chroma_y_end = bounds.y_end >> 1;

  for (int cy = chroma_y_start; cy <= chroma_y_end; ++cy) {
    uint8_t* u_row = frame->data[1] + cy * frame->linesize[1];
    uint8_t* v_row = frame->data[2] + cy * frame->linesize[2];

    fill_horizontal_line_fast(u_row, chroma_x_start, chroma_x_end, color.u);
    fill_horizontal_line_fast(v_row, chroma_x_start, chroma_x_end, color.v);
  }
}

void draw_rectangles_yuv420p_batch(AVFrame* frame,
                                   const std::vector<cv::Rect>& rects,
                                   const YUVColor& color,
                                   int thickness) {
  if (!frame || thickness <= 0 || rects.empty()) return;

  const auto fmt = static_cast<AVPixelFormat>(frame->format);
  if (!is_yuv420_format(fmt)) return;

  // Process rectangles in batches for better cache locality
  for (const auto& rect : rects) {
    draw_rectangle_yuv420p_optimized(frame, rect, color, thickness);
  }
}

void draw_rectangle_yuva420p_optimized(AVFrame* frame, const cv::Rect& rect,
                                       const YUVAColor& color, int thickness,
                                       bool write_chroma) {
  if (thickness <= 0 || !frame) return;

  if (!is_yuva420_format(static_cast<AVPixelFormat>(frame->format))) return;

  const auto bounds = BoundingRect::clip_to_frame(
      rect.x, rect.y, rect.width, rect.height,
      frame->width, frame->height);

  if (!bounds.is_valid()) return;

  // If thickness is too large, fill the entire rectangle
  if (thickness >= std::min(rect.width, rect.height) / 2) {
    draw_filled_rectangle_yuva420p_optimized(frame, rect, color, write_chroma);
    return;
  }

  // Draw four edges using line primitives
  for (int t = 0; t < thickness; ++t) {
    // Top and bottom edges
    for (int x = bounds.x_start; x <= bounds.x_end; ++x) {
      if (bounds.y_start + t <= bounds.y_end) {
        paint_pixel_yuva420p(frame, x, bounds.y_start + t, color, write_chroma);
      }
      if (bounds.y_end - t >= bounds.y_start && bounds.y_end - t != bounds.
          y_start + t) {
        paint_pixel_yuva420p(frame, x, bounds.y_end - t, color, write_chroma);
      }
    }

    // Left and right edges (excluding corners)
    for (int y = bounds.y_start + thickness; y <= bounds.y_end - thickness; ++
         y) {
      if (bounds.x_start + t <= bounds.x_end) {
        paint_pixel_yuva420p(frame, bounds.x_start + t, y, color, write_chroma);
      }
      if (bounds.x_end - t >= bounds.x_start && bounds.x_end - t != bounds.
          x_start + t) {
        paint_pixel_yuva420p(frame, bounds.x_end - t, y, color, write_chroma);
      }
    }
  }
}

void draw_filled_rectangle_yuva420p_optimized(AVFrame* frame,
                                              const cv::Rect& rect,
                                              const YUVAColor& color,
                                              bool write_chroma) {
  if (!frame) return;

  if (!is_yuva420_format(static_cast<AVPixelFormat>(frame->format))) return;

  const auto bounds = BoundingRect::clip_to_frame(
      rect.x, rect.y, rect.width, rect.height,
      frame->width, frame->height);

  if (!bounds.is_valid()) return;

  // Fill Y and A planes
  for (int y = bounds.y_start; y <= bounds.y_end; ++y) {
    uint8_t* y_row = frame->data[0] + y * frame->linesize[0];
    uint8_t* a_row = frame->data[3] + y * frame->linesize[3];

    fill_horizontal_line_fast(y_row, bounds.x_start, bounds.x_end, color.y);

    // Alpha blending - use max for overlapping elements
    for (int x = bounds.x_start; x <= bounds.x_end; ++x) {
      a_row[x] = std::max(a_row[x], color.a);
    }
  }

  if (write_chroma) {
    // Fill chroma planes (subsampled)
    const int chroma_x_start = bounds.x_start >> 1;
    const int chroma_x_end = bounds.x_end >> 1;
    const int chroma_y_start = bounds.y_start >> 1;
    const int chroma_y_end = bounds.y_end >> 1;

    for (int cy = chroma_y_start; cy <= chroma_y_end; ++cy) {
      uint8_t* u_row = frame->data[1] + cy * frame->linesize[1];
      uint8_t* v_row = frame->data[2] + cy * frame->linesize[2];

      fill_horizontal_line_fast(u_row, chroma_x_start, chroma_x_end, color.u);
      fill_horizontal_line_fast(v_row, chroma_x_start, chroma_x_end, color.v);
    }
  }
}

// Optimized Bresenham line algorithm
void draw_line_yuva420p_optimized(AVFrame* frame, int x0, int y0, int x1,
                                  int y1,
                                  const YUVAColor& color, int thickness,
                                  bool write_chroma) {
  if (!frame || thickness <= 0) return;

  if (!is_yuva420_format(static_cast<AVPixelFormat>(frame->format))) return;

  const int dx = std::abs(x1 - x0);
  const int sx = x0 < x1 ? 1 : -1;
  const int dy = -std::abs(y1 - y0);
  const int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  const int radius = thickness / 2;

  int x = x0, y = y0;
  while (true) {
    // Draw thick point using circle
    if (radius <= 1) {
      paint_pixel_yuva420p(frame, x, y, color, write_chroma);
    } else {
      draw_filled_circle_yuva420p_optimized(frame, x, y, radius, color,
                                            write_chroma);
    }

    if (x == x1 && y == y1) break;

    const int e2 = err << 1;
    if (e2 >= dy) {
      err += dy;
      x += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y += sy;
    }
  }
}

void draw_polygon_yuva420p_optimized(AVFrame* frame,
                                     const std::vector<cv::Point>& points,
                                     const YUVAColor& color, int thickness,
                                     bool write_chroma) {
  if (points.size() < 2) return;

  // Draw lines between consecutive points
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& p1 = points[i];
    const auto& p2 = points[(i + 1) % points.size()];
    draw_line_yuva420p_optimized(frame, p1.x, p1.y, p2.x, p2.y,
                                 color, thickness, write_chroma);
  }
}

void draw_circle_yuva420p_optimized(AVFrame* frame, int center_x, int center_y,
                                    int radius, const YUVAColor& color,
                                    int thickness, bool write_chroma) {
  if (!frame || radius <= 0 || thickness <= 0) return;

  if (!is_yuva420_format(static_cast<AVPixelFormat>(frame->format))) return;

  // Midpoint circle algorithm with thickness
  const int outer_radius = radius + thickness / 2;
  const int inner_radius = std::max(0, radius - thickness / 2);

  for (int y = -outer_radius; y <= outer_radius; ++y) {
    for (int x = -outer_radius; x <= outer_radius; ++x) {
      const int dist_sq = x * x + y * y;
      const int outer_sq = outer_radius * outer_radius;
      const int inner_sq = inner_radius * inner_radius;

      if (dist_sq <= outer_sq && dist_sq >= inner_sq) {
        paint_pixel_yuva420p(frame, center_x + x, center_y + y,
                             color, write_chroma);
      }
    }
  }
}

void draw_filled_circle_yuva420p_optimized(AVFrame* frame, int center_x,
                                           int center_y,
                                           int radius, const YUVAColor& color,
                                           bool write_chroma) {
  if (!frame || radius <= 0) return;

  if (!is_yuva420_format(static_cast<AVPixelFormat>(frame->format))) return;

  const int radius_sq = radius * radius;

  // Optimized circle filling using horizontal lines
  for (int y = -radius; y <= radius; ++y) {
    const int dy_sq = y * y;
    const int max_x = static_cast<int>(std::sqrt(radius_sq - dy_sq));

    const int pixel_y = center_y + y;
    if (!in_bounds(0, pixel_y, frame->width, frame->height)) continue;

    // Draw horizontal line from -max_x to +max_x
    for (int x = -max_x; x <= max_x; ++x) {
      paint_pixel_yuva420p(frame, center_x + x, pixel_y, color, write_chroma);
    }
  }
}

// YUV420P polygon drawing (no alpha channel)
void draw_polygon_yuv420p_optimized(AVFrame* frame,
                                    const std::vector<cv::Point>& points,
                                    const YUVColor& color, int thickness) {
  if (points.size() < 2 || !frame || thickness <= 0) return;

  const auto fmt = static_cast<AVPixelFormat>(frame->format);
  if (!is_yuv420_format(fmt)) return;

  // Simple Bresenham line algorithm for each edge
  auto draw_line = [&](int x0, int y0, int x1, int y1) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    const int radius = thickness / 2;
    int x = x0, y = y0;
    
    while (true) {
      // Draw thick point
      for (int dy_offset = -radius; dy_offset <= radius; ++dy_offset) {
        for (int dx_offset = -radius; dx_offset <= radius; ++dx_offset) {
          const int px = x + dx_offset;
          const int py = y + dy_offset;
          if (in_bounds(px, py, frame->width, frame->height)) {
            // Y plane
            frame->data[0][py * frame->linesize[0] + px] = color.y;
            
            // Chroma planes (subsampled)
            const int cx = px >> 1;
            const int cy = py >> 1;
            frame->data[1][cy * frame->linesize[1] + cx] = color.u;
            frame->data[2][cy * frame->linesize[2] + cx] = color.v;
          }
        }
      }

      if (x == x1 && y == y1) break;

      const int e2 = err << 1;
      if (e2 >= dy) {
        err += dy;
        x += sx;
      }
      if (e2 <= dx) {
        err += dx;
        y += sy;
      }
    }
  };

  // Draw lines between consecutive points
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& p1 = points[i];
    const auto& p2 = points[(i + 1) % points.size()];
    draw_line(p1.x, p1.y, p2.x, p2.y);
  }
}

} // namespace avs_analyzer