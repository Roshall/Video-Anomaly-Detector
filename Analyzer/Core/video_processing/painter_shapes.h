#ifndef ANALYZER_PAINTER_SHAPES_H
#define ANALYZER_PAINTER_SHAPES_H

#include "painter_base.h"
#include "painter_primitives.h"
#include "painter_types.h"
#include <vector>
#include "opencv2/core/types.hpp"

namespace avs_analyzer {
// Rectangle drawing functions
void draw_rectangle_yuv420p_optimized(AVFrame* frame, const cv::Rect& rect,
                                      const YUVColor& color, int thickness);

void draw_filled_rectangle_yuv420p_optimized(AVFrame* frame,
                                             const cv::Rect& rect,
                                             const YUVColor& color);

// Multiple rectangles with SIMD optimization
void draw_rectangles_yuv420p_batch(AVFrame* frame,
                                   const std::vector<cv::Rect>& rects,
                                   const YUVColor& color,
                                   int thickness);

// YUVA420P versions
void draw_rectangle_yuva420p_optimized(AVFrame* frame, const cv::Rect& rect,
                                       const YUVAColor& color, int thickness,
                                       bool write_chroma = true);

void draw_filled_rectangle_yuva420p_optimized(AVFrame* frame,
                                              const cv::Rect& rect,
                                              const YUVAColor& color,
                                              bool write_chroma = true);

// Line drawing
void draw_line_yuva420p_optimized(AVFrame* frame, int x0, int y0, int x1,
                                  int y1,
                                  const YUVAColor& color, int thickness = 2,
                                  bool write_chroma = true);

// Polygon drawing
void draw_polygon_yuva420p_optimized(AVFrame* frame,
                                     const std::vector<cv::Point>& points,
                                     const YUVAColor& color, int thickness = 2,
                                     bool write_chroma = true);

// Circle/disk drawing
void draw_circle_yuva420p_optimized(AVFrame* frame, int center_x, int center_y,
                                    int radius, const YUVAColor& color,
                                    int thickness = 2,
                                    bool write_chroma = true);

void draw_filled_circle_yuva420p_optimized(AVFrame* frame, int center_x,
                                           int center_y,
                                           int radius, const YUVAColor& color,
                                           bool write_chroma = true);
} // namespace avs_analyzer

#endif  // ANALYZER_PAINTER_SHAPES_H