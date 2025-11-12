//
// Created by lg on 11/12/25.
//
// Legend drawer for AV_PIX_FMT_YUVA420P using FreeType2
// Inputs:
//   frame            - AVFrame* in AV_PIX_FMT_YUVA420P (writable)
//   font_path        - path to .ttf / .otf
//   origin           - (x,y) top-left of the legend block
//   font_px          - font pixel height (FreeType "pixel size")
//   handle_wh        - (width, height) of the colored rectangle handle
//   row_spacing      - extra pixels between rows (>=0)
//   colors           - vector<YUVAColor> (y,u,v,a) for each legend item
//   labels_utf8      - vector<string> UTF-8, aligned with colors
// Notes:
//   - For crisp text edges, we set YUV to the legend color and take glyph coverage into A.
//   - If you want colored chroma-less text (sharper), you can skip painting U/V per pixel.
//   - This draws into a YUVA *overlay* (alpha used later by the overlay filter).
//     If you draw onto your final image (no later blending), alpha has no effect visually.
#include "painter.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Utils/log.h"
#include "freetype/freetype.h"
#include "freetype/ftimage.h"
#include "opencv2/core/types.hpp"


extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace avs_analyzer {
struct ChromaInfo {
  unsigned x;
  unsigned y;
  unsigned width;
  unsigned height;
};

// Bounds-checked paint of one pixel in Y + A; optionally paint subsampled U/V
inline void paint_px_yuva420p(AVFrame* f, unsigned x, unsigned y,
                              const YUVAColor& c,
                              bool paint_chroma = true) {
  if (x >= static_cast<unsigned>(f->width) || y >= static_cast<unsigned>(f->
        height))
    return;

  // Y and A planes are full resolution
  f->data[0][y * f->linesize[0] + x] = c.y; // Y
  f->data[3][y * f->linesize[3] + x] = c.a; // A

  if (paint_chroma) {
    // U/V are half resolution (4:2:0)
    const unsigned X = x >> 1, Y = y >> 1;
    // Guard in case of odd sizes/strides
    const unsigned maxUw = (f->width + 1) >> 1;
    const unsigned maxUh = (f->height + 1) >> 1;
    if (X < maxUw && Y < maxUh) {
      f->data[1][Y * f->linesize[1] + X] = c.u; // U
      f->data[2][Y * f->linesize[2] + X] = c.v; // V
    }
  }
}

// Stamp a disk brush of radius r around (x0,y0)
inline void stamp_brush_disk(AVFrame* f, int x0, int y0, int r,
                             const YUVAColor& c, bool paint_chroma) {
  if (r <= 0) {
    paint_px_yuva420p(f, x0, y0, c, paint_chroma);
    return;
  }
  int r2 = r * r;
  for (int oy = -r; oy <= r; ++oy) {
    int yy = y0 + oy;
    int oy2 = oy * oy;
    for (int ox = -r; ox <= r; ++ox) {
      if (ox * ox + oy2 > r2) continue;
      int xx = x0 + ox;
      paint_px_yuva420p(f, xx, yy, c, paint_chroma);
    }
  }
}

// Thickness-aware Bresenham line. thickness in pixels (>=1).
// Set paint_chroma=false for crisper thin lines (Y-only); true for colored
// lines.
inline void draw_line_yuva420p_thick(AVFrame* f, int x0, int y0, int x1, int y1,
                                     const YUVAColor& c, int thickness = 2,
                                     bool paint_chroma = true) {
  // Require YUVA420P
  if (f->format != AV_PIX_FMT_YUVA420P) return;

  int r = std::max(0, thickness / 2);

  int dx = std::abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  while (true) {
    stamp_brush_disk(f, x0, y0, r, c, paint_chroma);
    if (x0 == x1 && y0 == y1) break;
    int e2 = err << 1;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

inline void draw_rectangle_yuva420p_thick(AVFrame* f, const cv::Rect& r,
                                          const YUVAColor& c, int thickness,
                                          bool paint_chroma) {
  if (f->format != AV_PIX_FMT_YUVA420P) return; // Or handle other formats
  int x1 = r.x;
  int y1 = r.y;
  int x2 = r.x + r.width;
  int y2 = r.y + r.height;

  draw_line_yuva420p_thick(f, x1, y1, x2, y1, c, thickness,
                           paint_chroma); // Top
  draw_line_yuva420p_thick(f, x2, y1, x2, y2, c, thickness,
                           paint_chroma); // Right
  draw_line_yuva420p_thick(f, x2, y2, x1, y2, c, thickness,
                           paint_chroma); // Bottom
  draw_line_yuva420p_thick(f, x1, y2, x1, y1, c, thickness,
                           paint_chroma); // Left
}

/**
 * @brief Optimized horizontal line drawing
 */
inline void draw_horizontal_line_yuv420p(AVFrame* f, int x1, int x2, int y,
                                         const YUVColor& color, int thickness) {
  const int start_x = std::max(x1, 0);
  const int end_x = std::min(x2, f->width - 1);
  const int start_y = std::max(y, 0);
  const int end_y = std::min(y + thickness - 1, f->height - 1);

  if (end_x < start_x || end_y < start_y) return;

  // Draw luma (Y) plane
  for (int y_pos = start_y; y_pos <= end_y; ++y_pos) {
    uint8_t* y_plane = f->data[0] + y_pos * f->linesize[0];
    std::memset(y_plane + start_x, color.y, end_x - start_x + 1);
  }

  // Calculate chroma coordinates once per block
  const int chroma_start_x = start_x / 2;
  const int chroma_end_x = end_x / 2;
  const int chroma_start_y = start_y / 2;
  const int chroma_end_y = end_y / 2;

  // Draw chroma planes
  for (int cy = chroma_start_y; cy <= chroma_end_y; ++cy) {
    uint8_t* u_plane = f->data[1] + cy * f->linesize[1];
    uint8_t* v_plane = f->data[2] + cy * f->linesize[2];

    for (int cx = chroma_start_x; cx <= chroma_end_x; ++cx) {
      u_plane[cx] = color.u;
      v_plane[cx] = color.v;
    }
  }
}


inline void draw_vertical_line_yuv420p(AVFrame* f, int x, int y1, int y2,
                                       const YUVColor& color, int thickness) {
  const int start_x = std::max(x, 0);
  const int end_x = std::min(x + thickness - 1, f->width - 1);
  const int start_y = std::max(y1, 0);
  const int end_y = std::min(y2, f->height - 1);

  if (end_x < start_x || end_y < start_y) return;

  // Draw luma (Y) plane
  for (int y_pos = start_y; y_pos <= end_y; ++y_pos) {
    uint8_t* y_plane = f->data[0] + y_pos * f->linesize[0];
    for (int x_pos = start_x; x_pos <= end_x; ++x_pos) {
      y_plane[x_pos] = color.y;
    }
  }

  // Calculate chroma coordinates once per block
  const int chroma_start_x = start_x / 2;
  const int chroma_end_x = end_x / 2;
  const int chroma_start_y = start_y / 2;
  const int chroma_end_y = end_y / 2;

  // Draw chroma planes
  for (int cy = chroma_start_y; cy <= chroma_end_y; ++cy) {
    uint8_t* u_plane = f->data[1] + cy * f->linesize[1];
    uint8_t* v_plane = f->data[2] + cy * f->linesize[2];

    for (int cx = chroma_start_x; cx <= chroma_end_x; ++cx) {
      u_plane[cx] = color.u;
      v_plane[cx] = color.v;
    }
  }
}

inline void draw_filled_rectangle_yuv420p(AVFrame* f, const cv::Rect& rect,
                                          const YUVColor& color) {
  const int start_x = std::max(rect.x, 0);
  const int end_x = std::min(rect.x + rect.width - 1, f->width - 1);
  const int start_y = std::max(rect.y, 0);
  const int end_y = std::min(rect.y + rect.height - 1, f->height - 1);

  if (end_x < start_x || end_y < start_y) return;

  // Fill luma plane
  for (int y = start_y; y <= end_y; ++y) {
    uint8_t* y_plane = f->data[0] + y * f->linesize[0];
    std::memset(y_plane + start_x, color.y, end_x - start_x + 1);
  }

  // Calculate chroma coordinates
  const int chroma_start_x = start_x / 2;
  const int chroma_end_x = end_x / 2;
  const int chroma_start_y = start_y / 2;
  const int chroma_end_y = end_y / 2;

  // Fill chroma planes
  for (int cy = chroma_start_y; cy <= chroma_end_y; ++cy) {
    uint8_t* u_plane = f->data[1] + cy * f->linesize[1];
    uint8_t* v_plane = f->data[2] + cy * f->linesize[2];

    for (int cx = chroma_start_x; cx <= chroma_end_x; ++cx) {
      u_plane[cx] = color.u;
      v_plane[cx] = color.v;
    }
  }
}

inline void draw_rectangle_yuv420p(AVFrame* f, const cv::Rect& rect,
                                   const YUVColor& color, int thickness) {
  if (thickness <= 0) return;

  const auto fmt = static_cast<AVPixelFormat>(f->format);
  if (fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_YUVJ420P) {
    LOGW(
        "the pixel format code [{}] is neither AV_PIX_FMT_YUV420P nor AV_PIX_FMT_YUVJ420P",
        f->format);
  }

  const int width = f->width;
  const int height = f->height;

  // Clamp rectangle to frame boundaries
  const int x1 = std::clamp(rect.x, 0, width - 1);
  const int y1 = std::clamp(rect.y, 0, height - 1);
  const int x2 = std::clamp(rect.x + rect.width - 1, 0, width - 1);
  const int y2 = std::clamp(rect.y + rect.height - 1, 0, height - 1);

  if (x2 < x1 || y2 < y1) return;

  if (thickness >= std::min(rect.width, rect.height) / 2) {
    draw_filled_rectangle_yuv420p(f, rect, color);
    return;
  }

  // Draw four edges as rectangles
  // Top edge
  if (y1 >= 0 && y1 < height) {
    draw_horizontal_line_yuv420p(f, x1, x2, y1, color, thickness);
  }

  // Bottom edge
  const int bottom_y = y2 - thickness + 1;
  if (bottom_y >= 0 && bottom_y < height) {
    draw_horizontal_line_yuv420p(f, x1, x2, bottom_y, color, thickness);
  }

  // Left edge (excluding corners already drawn)
  const int left_start_y = y1 + thickness;
  const int left_end_y = y2 - thickness;
  if (left_end_y >= left_start_y) {
    draw_vertical_line_yuv420p(f, x1, left_start_y, left_end_y, color,
                               thickness);
  }

  // Right edge (excluding corners already drawn)
  const int right_start_y = y1 + thickness;
  const int right_end_y = y2 - thickness;
  if (right_end_y >= right_start_y) {
    draw_vertical_line_yuv420p(f, x2 - thickness + 1, right_start_y,
                               right_end_y, color, thickness);
  }
}

// Polygon outline with thickness
void draw_polygon_yuva420p_thick(AVFrame* f,
                                 const std::vector<cv::Point>& poly,
                                 const YUVAColor& c, int thickness,
                                 bool paint_chroma) {
  if (poly.size() < 2) return;
  for (size_t i = 0; i < poly.size(); ++i) {
    const auto& a = poly[i];
    const auto& b = poly[(i + 1) % poly.size()];
    draw_line_yuva420p_thick(f, a.x, a.y, b.x, b.y, c, thickness, paint_chroma);
  }
}

void draw_horizontal_line_y_simd(uint8_t* row, int x1, int x2, uint8_t val) {
#pragma omp simd aligned(row:32)
  for (int i = x1; i <= x2; ++i)
    row[i] = val;
}


void draw_rectangles_yuv420p_fast(AVFrame* f,
                                  const std::vector<cv::Rect>& rects,
                                  const YUVColor& color,
                                  int thickness) {
  if (!f || thickness <= 0) return;
  if (const auto fmt = static_cast<AVPixelFormat>(f->format);
    fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_YUVJ420P) {
    LOGW(
        "the pixel format code [{}] is neither AV_PIX_FMT_YUV420P nor AV_PIX_FMT_YUVJ420P",
        f->format);
  }

  const int width = f->width;
  const int height = f->height;

  uint8_t* Y = f->data[0];
  uint8_t* U = f->data[1];
  uint8_t* V = f->data[2];
  const int Y_stride = f->linesize[0];
  const int U_stride = f->linesize[1];
  const int V_stride = f->linesize[2];

  for (const auto& rect : rects) {
    int x1 = std::clamp(rect.x, 0, width - 1);
    int y1 = std::clamp(rect.y, 0, height - 1);
    int x2 = std::clamp(rect.x + rect.width - 1, 0, width - 1);
    int y2 = std::clamp(rect.y + rect.height - 1, 0, height - 1);
    if (x2 <= x1 || y2 <= y1) continue;

    // top and bottom edges (Y)
    for (int t = 0; t < thickness; ++t) {
      if (y1 + t < height)
        draw_horizontal_line_y_simd(Y + (y1 + t) * Y_stride, x1, x2, color.y);
      if (y2 - t >= 0 && y2 - t < height)
        draw_horizontal_line_y_simd(Y + (y2 - t) * Y_stride, x1, x2, color.y);
    }

    // left and right edges (Y)
    for (int y = y1 + thickness; y <= y2 - thickness; ++y) {
      uint8_t* row = Y + y * Y_stride;
      for (int t = 0; t < thickness; ++t) {
        if (x1 + t < width) row[x1 + t] = color.y;
        if (x2 - t >= 0 && x2 - t < width) row[x2 - t] = color.y;
      }
    }

    // subsampled UV planes
    int x1c = x1 / 2, x2c = x2 / 2;
    int y1c = y1 / 2, y2c = y2 / 2;
    for (int t = 0; t < std::max(1, thickness / 2); ++t) {
      if (y1c + t < height / 2) {
        draw_horizontal_line_y_simd(U + (y1c + t) * U_stride, x1c, x2c,
                                    color.u);
        draw_horizontal_line_y_simd(V + (y1c + t) * V_stride, x1c, x2c,
                                    color.v);
      }
      if (y2c - t >= 0 && y2c - t < height / 2) {
        draw_horizontal_line_y_simd(U + (y2c - t) * U_stride, x1c, x2c,
                                    color.u);
        draw_horizontal_line_y_simd(V + (y2c - t) * V_stride, x1c, x2c,
                                    color.v);
      }
    }
    for (int yc = y1c + thickness / 2; yc <= y2c - thickness / 2; ++yc) {
      uint8_t* rowU = U + yc * U_stride;
      uint8_t* rowV = V + yc * V_stride;
      for (int t = 0; t < std::max(1, thickness / 2); ++t) {
        if (x1c + t < width / 2) {
          rowU[x1c + t] = color.u;
          rowV[x1c + t] = color.v;
        }
        if (x2c - t >= 0 && x2c - t < width / 2) {
          rowU[x2c - t] = color.u;
          rowV[x2c - t] = color.v;
        }
      }
    }
  }
}

static inline bool in_bounds(int x, int y, int width, int height) {
  return static_cast<unsigned>(x) < static_cast<unsigned>(width)
         && static_cast<unsigned>(y) < static_cast<unsigned>(height);
}

// Get chroma plane information for 4:2:0 subsampling
static inline ChromaInfo get_chroma_info(const AVFrame* frame, int x, int y) {
  return {
      static_cast<unsigned>(x >> 1),
      static_cast<unsigned>(y >> 1),
      static_cast<unsigned>((frame->width + 1) >> 1),
      static_cast<unsigned>((frame->height + 1) >> 1)
  };
}

// Set chroma pixels if within bounds
static inline void set_chroma_pixels(AVFrame* frame, unsigned chroma_x,
                                     unsigned chroma_y,
                                     uint8_t u_value, uint8_t v_value) {
  if (chroma_x < get_chroma_info(frame, frame->width, 0).width &&
      chroma_y < get_chroma_info(frame, 0, frame->height).height) {
    frame->data[1][chroma_y * frame->linesize[1] + chroma_x] = u_value;
    frame->data[2][chroma_y * frame->linesize[2] + chroma_x] = v_value;
  }
}

// Blend alpha with coverage scaled by color alpha
static inline void write_alpha_coverage(AVFrame* frame, int x, int y,
                                        uint8_t coverage, uint8_t color_alpha) {
  if (!in_bounds(x, y, frame->width, frame->height)) return;

  uint8_t* alpha_ptr = frame->data[3] + y * frame->linesize[3] + x;
  const auto blended_alpha = static_cast<uint32_t>(coverage) * static_cast<
                               uint32_t>(color_alpha) / 255u;

  // Keep maximum alpha for overlapping elements
  *alpha_ptr = std::max(*alpha_ptr, static_cast<uint8_t>(blended_alpha));
}

// Draw filled rectangle
static void fill_rect_yuva420p(AVFrame* frame, int x, int y, int width,
                               int height,
                               const YUVAColor& color,
                               bool write_chroma = true) {
  const int x_start = std::max(0, x);
  const int y_start = std::max(0, y);
  const int x_end = std::min(frame->width, x + width);
  const int y_end = std::min(frame->height, y + height);

  // Luma and alpha planes (full resolution)
  for (int row = y_start; row < y_end; ++row) {
    for (int col = x_start; col < x_end; ++col) {
      frame->data[0][row * frame->linesize[0] + col] = color.y;

      uint8_t& alpha_ref = frame->data[3][row * frame->linesize[3] + col];
      alpha_ref = std::max(alpha_ref, color.a);
    }
  }

  if (write_chroma) {
    // Chroma planes (half resolution)
    const int chroma_x_start = x_start >> 1;
    const int chroma_y_start = y_start >> 1;
    const int chroma_x_end = (x_end + 1) >> 1;
    const int chroma_y_end = (y_end + 1) >> 1;

    for (int chroma_row = chroma_y_start; chroma_row < chroma_y_end; ++
         chroma_row) {
      for (int chroma_col = chroma_x_start; chroma_col < chroma_x_end; ++
           chroma_col) {
        set_chroma_pixels(frame, chroma_col, chroma_row, color.u, color.v);
      }
    }
  }
}

// Draw rectangle outline
static void draw_rect_outline_yuva420p(AVFrame* frame, int x, int y, int width,
                                       int height,
                                       const YUVAColor& color,
                                       int thickness = 2,
                                       bool write_chroma = true) {
  if (thickness <= 0 || width <= 0 || height <= 0) return;

  // Top, bottom, left, right strips
  fill_rect_yuva420p(frame, x, y, width, thickness, color, write_chroma);
  fill_rect_yuva420p(frame, x, y + height - thickness, width, thickness, color,
                     write_chroma);
  fill_rect_yuva420p(frame, x, y, thickness, height, color, write_chroma);
  fill_rect_yuva420p(frame, x + width - thickness, y, thickness, height, color,
                     write_chroma);
}

// UTF-8 to codepoint decoder
static bool utf8_next(const std::u8string& str, size_t& index,
                      uint32_t& codepoint) {
  if (index >= str.size()) return false;

  const char8_t lead_byte = str[index++];

  // Single byte character (ASCII)
  if (lead_byte < 0x80) {
    codepoint = lead_byte;
    return true;
  }

  // Multi-byte character
  int extra_bytes = -1;
  if ((lead_byte & 0xE0) == 0xC0) extra_bytes = 1;
  else if ((lead_byte & 0xF0) == 0xE0) extra_bytes = 2;
  else if ((lead_byte & 0xF8) == 0xF0) extra_bytes = 3;

  if (extra_bytes < 0 || index + extra_bytes > str.size()) return false;

  codepoint = lead_byte & (0x1F >> (extra_bytes - 1));

  for (int i = 0; i < extra_bytes; ++i) {
    const char8_t trail_byte = str[index++];
    if ((trail_byte & 0xC0) != 0x80) return false;
    codepoint = (codepoint << 6) | (trail_byte & 0x3F);
  }

  return true;
}

// Render glyph bitmap to YUVA frame
static void blit_glyph_gray_to_yuva420p(AVFrame* frame, int dest_x, int dest_y,
                                        const FT_Bitmap& bitmap,
                                        const YUVAColor& color,
                                        bool write_chroma = true) {
  for (int row = 0; row < bitmap.rows; ++row) {
    for (int col = 0; col < bitmap.width; ++col) {
      const int pixel_x = dest_x + col;
      const int pixel_y = dest_y + row;

      if (!in_bounds(pixel_x, pixel_y, frame->width, frame->height)) continue;

      const uint8_t coverage = bitmap.buffer[row * bitmap.pitch + col];
      if (coverage == 0) continue;

      // Set luma to text color
      frame->data[0][pixel_y * frame->linesize[0] + pixel_x] = color.y;

      // Blend alpha with glyph coverage
      write_alpha_coverage(frame, pixel_x, pixel_y, coverage, color.a);

      if (write_chroma) {
        const auto chroma = get_chroma_info(frame, pixel_x, pixel_y);
        set_chroma_pixels(frame, chroma.x, chroma.y, color.u, color.v);
      }
    }
  }
}

// Measure text width using FreeType advances
static int measure_text_width(FT_Face face, const std::u8string& utf8_text) {
  int width = 0;
  size_t index = 0;
  uint32_t codepoint;

  while (utf8_next(utf8_text, index, codepoint)) {
    if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT)) continue;
    width += static_cast<int>(face->glyph->advance.x >> 6);
  }

  return width;
}

// Initialize FreeType and load font face
static bool initialize_freetype(FT_Library& library, FT_Face& face,
                                const std::string& font_path, int font_size) {
  if (FT_Init_FreeType(&library)) return false;
  if (FT_New_Face(library, font_path.c_str(), 0, &face)) {
    FT_Done_FreeType(library);
    return false;
  }
  if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(font_size))) {
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return false;
  }
  return true;
}

// Calculate legend dimensions
static std::tuple<int, int, int, int> calculate_legend_dimensions(
    FT_Face face, const std::vector<std::u8string>& labels,
    std::pair<int, int> origin, std::pair<int, int> handle_size,
    int font_size, int row_spacing, int text_padding, int frame_padding) {
  const int start_x = origin.first;
  const int start_y = origin.second;
  const int handle_width = std::max(0, handle_size.first);
  const int handle_height = std::max(0, handle_size.second);
  const int row_height = std::max(handle_height, font_size);

  // Measure text dimensions
  int max_text_width = 0;
  for (const auto& label : labels) {
    max_text_width = std::max(max_text_width, measure_text_width(face, label));
  }

  // Calculate content dimensions
  const int item_count = static_cast<int>(labels.size());
  const int content_width = handle_width + text_padding + max_text_width;
  const int content_height = item_count > 0
                               ? row_height * item_count + std::max(
                                     0, row_spacing) * (item_count - 1)
                               : 0;

  // Calculate frame dimensions
  int frame_x = std::max(0, start_x - frame_padding);
  int frame_y = std::max(0, start_y - frame_padding - font_size / 2);
  int frame_width = content_width + 2 * frame_padding;
  int frame_height = content_height + 2 * frame_padding;

  return {frame_x, frame_y, frame_width, frame_height};
}

// Draw a single legend item
static void draw_legend_item(AVFrame* frame, FT_Face face, int start_x,
                             int current_y,
                             const YUVAColor& color, const std::u8string& label,
                             int handle_width, int handle_height, int font_size,
                             int text_padding, bool write_chroma) {
  // Draw color handle
  fill_rect_yuva420p(frame, start_x, current_y, handle_width, handle_height,
                     color, write_chroma);

  // Draw text label
  const int text_x = start_x + handle_width + text_padding;
  const int baseline_y = current_y + font_size / 2;

  size_t char_index = 0;
  uint32_t codepoint;
  int pen_x = text_x;

  while (utf8_next(label, char_index, codepoint)) {
    if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER)) continue;

    FT_GlyphSlot glyph = face->glyph;
    const int glyph_x = pen_x + glyph->bitmap_left;
    const int glyph_y = baseline_y - glyph->bitmap_top;

    blit_glyph_gray_to_yuva420p(frame, glyph_x, glyph_y, glyph->bitmap, color,
                                write_chroma);
    pen_x += static_cast<int>(glyph->advance.x >> 6);
  }
}

inline AVFrame* create_blank_yuva420p(int width, int height) {
  AVFrame* f = av_frame_alloc();
  if (!f) throw std::runtime_error("Failed to allocate AVFrame");

  f->format = AV_PIX_FMT_YUVA420P;
  f->width = width;
  f->height = height;
  if (av_frame_get_buffer(f, 32) < 0)
    throw std::runtime_error("av_frame_get_buffer failed");

  // Fill planes: neutral gray (Y=16+219/2≈128), chroma center (U=128,V=128), alpha=0 (transparent)
  for (int y = 0; y < f->height; ++y)
    memset(f->data[0] + y * f->linesize[0], 128, f->width);
  for (int y = 0; y < (f->height + 1) / 2; ++y) {
    memset(f->data[1] + y * f->linesize[1], 128, (f->width + 1) / 2);
    memset(f->data[2] + y * f->linesize[2], 128, (f->width + 1) / 2);
  }
  for (int y = 0; y < f->height; ++y)
    memset(f->data[3] + y * f->linesize[3], 0, f->width); // fully transparent

  return f;
}

static inline bool utf8_next(const std::string& s, size_t& i, uint32_t& cp) {
  if (i >= s.size()) return false;
  unsigned char c = s[i++];
  if (c < 0x80) {
    cp = c;
    return true;
  }
  if ((c >> 5) == 0x6 && i < s.size()) {
    cp = ((c & 0x1F) << 6) | (s[i++] & 0x3F);
    return true;
  }
  if ((c >> 4) == 0xE && i + 1 < s.size()) {
    cp = ((c & 0x0F) << 12) | ((s[i++] & 0x3F) << 6) | (s[i++] & 0x3F);
    return true;
  }
  if ((c >> 3) == 0x1E && i + 2 < s.size()) {
    cp = ((c & 7) << 18) | ((s[i++] & 0x3F) << 12) | ((s[i++] & 0x3F) << 6) | (
           s[i++] & 0x3F);
    return true;
  }
  return false;
}

// Measure width in pixels of UTF-8 string using FreeType face
static inline int measure_text_width_px(FT_Face face, const std::string& utf8) {
  int width = 0;
  size_t i = 0;
  uint32_t cp;
  while (utf8_next(utf8, i, cp)) {
    if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT)) continue;
    width += static_cast<int>(face->glyph->advance.x >> 6);
  }
  return width;
}

// Estimate bounding box of legend (including handles, padding, and frame)
inline std::pair<int, int> estimate_legend_size(
    const std::string& font_path,
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::string>& labels_utf8,
    int text_padding = 8,
    int frame_padding = 8) {
  FT_Library ft;
  if (FT_Init_FreeType(&ft))
    throw std::runtime_error("FreeType init failed");

  FT_Face face;
  if (FT_New_Face(ft, font_path.c_str(), 0, &face)) {
    FT_Done_FreeType(ft);
    throw std::runtime_error("Failed to load font: " + font_path);
  }
  FT_Set_Pixel_Sizes(face, 0, font_px);

  int max_text_w = 0;
  for (const auto& s : labels_utf8)
    max_text_w = std::max(max_text_w, measure_text_width_px(face, s));

  FT_Done_Face(face);
  FT_Done_FreeType(ft);

  const auto [handle_w, handle_h] = handle_wh;
  const int row_h = std::max(handle_h, font_px);
  const int item_count = static_cast<int>(labels_utf8.size());
  const int content_h = item_count > 0
                          ? row_h * item_count + row_spacing * (item_count - 1)
                          : 0;
  const int content_w = handle_w + text_padding + max_text_w;

  const int total_w = content_w + frame_padding * 2;
  const int total_h = content_h + frame_padding * 2;

  return {total_w, total_h};
}

inline LegendLayout estimate_legend_layout(
    const std::string& font_path,
    int font_px,
    std::pair<int, int> handle_wh,
    int row_spacing,
    const std::vector<std::string>& labels_utf8,
    int text_padding,
    int frame_padding,
    int video_w,
    int video_h,
    const LegendPosition position_mode = LegendPosition::kTopRight,
    int margin_x = 40,
    int margin_y = 40) {
  FT_Library ft;
  if (FT_Init_FreeType(&ft))
    throw std::runtime_error("FreeType init failed");

  FT_Face face;
  if (FT_New_Face(ft, font_path.c_str(), 0, &face)) {
    FT_Done_FreeType(ft);
    throw std::runtime_error("Failed to load font: " + font_path);
  }
  FT_Set_Pixel_Sizes(face, 0, font_px);

  int max_text_w = 0;
  for (const auto& s : labels_utf8)
    max_text_w = std::max(max_text_w, measure_text_width_px(face, s));

  FT_Done_Face(face);
  FT_Done_FreeType(ft);

  const auto [handle_w, handle_h] = handle_wh;
  const int row_h = std::max(handle_h, font_px);
  const int item_count = static_cast<int>(labels_utf8.size());
  const int content_h = item_count > 0
                          ? row_h * item_count + row_spacing * (item_count - 1)
                          : 0;
  const int content_w = handle_w + text_padding + max_text_w;

  const int total_w = content_w + frame_padding * 2;
  const int total_h = content_h + frame_padding * 2;

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

// Main legend drawing function
void draw_legend_yuva420p(AVFrame* frame,
                          const std::string& font_path,
                          std::pair<int, int> origin,
                          int font_size,
                          std::pair<int, int> handle_size,
                          int row_spacing,
                          const std::vector<YUVAColor>& colors,
                          const std::vector<std::u8string>& labels,
                          bool write_chroma,
                          int text_padding,
                          bool draw_frame,
                          int frame_padding,
                          int frame_thickness,
                          YUVAColor frame_color) {
  // Validate inputs
  if (!frame || frame->format != AV_PIX_FMT_YUVA420P) {
    throw std::runtime_error("Frame must be in AV_PIX_FMT_YUVA420P format");
  }

  if (colors.size() != labels.size()) {
    throw std::runtime_error("Colors and labels must have the same size");
  }

  // Initialize FreeType
  FT_Library free_type;
  FT_Face face;
  if (!initialize_freetype(free_type, face, font_path, font_size)) {
    throw std::runtime_error(
        "Failed to initialize FreeType or load font: " + font_path);
  }

  const int start_x = origin.first;
  const int handle_width = std::max(0, handle_size.first);
  const int handle_height = std::max(0, handle_size.second);
  const int row_height = std::max(handle_height, font_size);
  const int row_step = row_height + std::max(0, row_spacing);

  // Calculate dimensions
  auto [frame_x, frame_y, frame_width, frame_height] =
      calculate_legend_dimensions(
          face, labels, origin, handle_size, font_size, row_spacing,
          text_padding, frame_padding);

  // Clamp to frame boundaries
  frame_width = std::min(frame_width, frame->width - frame_x);
  frame_height = std::min(frame_height, frame->height - frame_y);

  // Draw legend items
  int current_y = origin.second;
  const int item_count = static_cast<int>(labels.size());

  for (int i = 0; i < item_count; ++i) {
    if (current_y >= frame->height) break;
    draw_legend_item(frame, face, start_x, current_y, colors[i], labels[i],
                     handle_width, handle_height, font_size, text_padding,
                     write_chroma);
    current_y += row_step;
  }

  // Draw frame around legend
  if (draw_frame && frame_width > 0 && frame_height > 0 && frame_thickness >
      0) {
    draw_rect_outline_yuva420p(frame, frame_x, frame_y, frame_width,
                               frame_height,
                               frame_color, frame_thickness, true);
  }

  // Cleanup FreeType resources
  FT_Done_Face(face);
  FT_Done_FreeType(free_type);
}
}