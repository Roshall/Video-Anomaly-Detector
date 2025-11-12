#ifndef ANALYZER_PAINTER_PRIMITIVES_H
#define ANALYZER_PAINTER_PRIMITIVES_H

#include "painter_base.h"
#include "painter_types.h"  // For color structs
#include <cstring>

namespace avs_analyzer {
// SIMD-optimized horizontal line drawing
inline void draw_horizontal_line_simd(uint8_t* row, int x_start, int x_end,
                                      uint8_t value) noexcept {
  if (x_end < x_start) return;

  const int length = x_end - x_start + 1;

#pragma omp simd aligned(row:32)
  for (int i = 0; i < length; ++i) {
    row[x_start + i] = value;
  }
}

// Optimized memset for larger regions
inline void fill_horizontal_line_fast(uint8_t* row, int x_start, int x_end,
                                      uint8_t value) noexcept {
  if (x_end < x_start) return;

  const int length = x_end - x_start + 1;
  if (length >= 16) {
    std::memset(row + x_start, value, length);
  } else {
    draw_horizontal_line_simd(row, x_start, x_end, value);
  }
}

// Optimized pixel painting for YUV420P
inline void paint_pixel_yuv420p(AVFrame* frame, int x, int y,
                                const YUVColor& color) noexcept {
  if (!in_bounds(x, y, frame->width, frame->height)) return;

  // Y plane (full resolution)
  frame->data[0][y * frame->linesize[0] + x] = color.y;

  // UV planes (half resolution)
  const int chroma_x = x >> 1;
  const int chroma_y = y >> 1;
  const int chroma_width = (frame->width + 1) >> 1;
  const int chroma_height = (frame->height + 1) >> 1;

  if (chroma_x < chroma_width && chroma_y < chroma_height) {
    frame->data[1][chroma_y * frame->linesize[1] + chroma_x] = color.u;
    frame->data[2][chroma_y * frame->linesize[2] + chroma_x] = color.v;
  }
}

// Optimized pixel painting for YUVA420P
inline void paint_pixel_yuva420p(AVFrame* frame, int x, int y,
                                 const YUVAColor& color,
                                 bool write_chroma = true) noexcept {
  if (!in_bounds(x, y, frame->width, frame->height)) return;

  // Y and A planes (full resolution)
  frame->data[0][y * frame->linesize[0] + x] = color.y;
  frame->data[3][y * frame->linesize[3] + x] = color.a;

  if (write_chroma) {
    const int chroma_x = x >> 1;
    const int chroma_y = y >> 1;
    const int chroma_width = (frame->width + 1) >> 1;
    const int chroma_height = (frame->height + 1) >> 1;

    if (chroma_x < chroma_width && chroma_y < chroma_height) {
      frame->data[1][chroma_y * frame->linesize[1] + chroma_x] = color.u;
      frame->data[2][chroma_y * frame->linesize[2] + chroma_x] = color.v;
    }
  }
}

// Optimized horizontal line for YUV420P
inline void draw_horizontal_line_yuv420p_optimized(
    AVFrame* frame, int x1, int x2, int y,
    const YUVColor& color, int thickness) noexcept {
  const auto bounds = BoundingRect::clip_to_frame(
      std::min(x1, x2), y,
      std::abs(x2 - x1) + 1, thickness,
      frame->width, frame->height);

  if (!bounds.is_valid()) return;

  // Draw Y plane
  for (int y_pos = bounds.y_start; y_pos <= bounds.y_end; ++y_pos) {
    uint8_t* y_row = frame->data[0] + y_pos * frame->linesize[0];
    fill_horizontal_line_fast(y_row, bounds.x_start, bounds.x_end, color.y);
  }

  // Draw chroma planes (subsampled)
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

// Optimized vertical line for YUV420P
inline void draw_vertical_line_yuv420p_optimized(
    AVFrame* frame, int x, int y1, int y2,
    const YUVColor& color, int thickness) noexcept {
  const auto bounds = BoundingRect::clip_to_frame(
      x, std::min(y1, y2),
      thickness, std::abs(y2 - y1) + 1,
      frame->width, frame->height);

  if (!bounds.is_valid()) return;

  // Draw Y plane
  for (int y_pos = bounds.y_start; y_pos <= bounds.y_end; ++y_pos) {
    uint8_t* y_row = frame->data[0] + y_pos * frame->linesize[0];

#pragma omp simd
    for (int x_pos = bounds.x_start; x_pos <= bounds.x_end; ++x_pos) {
      y_row[x_pos] = color.y;
    }
  }

  // Draw chroma planes (subsampled)
  const int chroma_x_start = bounds.x_start >> 1;
  const int chroma_x_end = bounds.x_end >> 1;
  const int chroma_y_start = bounds.y_start >> 1;
  const int chroma_y_end = bounds.y_end >> 1;

  for (int cy = chroma_y_start; cy <= chroma_y_end; ++cy) {
    uint8_t* u_row = frame->data[1] + cy * frame->linesize[1];
    uint8_t* v_row = frame->data[2] + cy * frame->linesize[2];

#pragma omp simd
    for (int cx = chroma_x_start; cx <= chroma_x_end; ++cx) {
      u_row[cx] = color.u;
      v_row[cx] = color.v;
    }
  }
}
} // namespace avs_analyzer

#endif  // ANALYZER_PAINTER_PRIMITIVES_H