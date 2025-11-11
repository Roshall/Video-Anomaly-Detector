//
// Created by lg on 10/2/25.
//

#include "annotator.h"

#include <format>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <opencv2/opencv.hpp>

#include <helper.h>
#include <Utils/log.h>

namespace avs_analyzer {
// In video_overlay.cpp

// Represents a YUVA color. Red is {76, 84, 255}. Alpha=255 is fully opaque.
struct YUVAColor {
  uint8_t y, u, v, a;
};

// Bounds-checked paint of one pixel in Y + A; optionally paint subsampled U/V
inline void paint_px_yuva420p(AVFrame* f, int x, int y, const YUVAColor& c,
                              bool paint_chroma = true) {
  if ((unsigned)x >= (unsigned)f->width || (unsigned)y >= (unsigned)f->height)
    return;

  // Y and A planes are full resolution
  f->data[0][y * f->linesize[0] + x] = c.y;  // Y
  f->data[3][y * f->linesize[3] + x] = c.a;  // A

  if (paint_chroma) {
    // U/V are half resolution (4:2:0)
    int X = x >> 1, Y = y >> 1;
    // Guard in case of odd sizes/strides
    int maxUw = (f->width + 1) >> 1;
    int maxUh = (f->height + 1) >> 1;
    if ((unsigned)X < (unsigned)maxUw && (unsigned)Y < (unsigned)maxUh) {
      f->data[1][Y * f->linesize[1] + X] = c.u;  // U
      f->data[2][Y * f->linesize[2] + X] = c.v;  // V
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
  if (f->format != AV_PIX_FMT_YUVA420P) return;  // Or handle other formats
  int x1 = r.x;
  int y1 = r.y;
  int x2 = r.x + r.width;
  int y2 = r.y + r.height;

  draw_line_yuva420p_thick(f, x1, y1, x2, y1, c, thickness,
                           paint_chroma);  // Top
  draw_line_yuva420p_thick(f, x2, y1, x2, y2, c, thickness,
                           paint_chroma);  // Right
  draw_line_yuva420p_thick(f, x2, y2, x1, y2, c, thickness,
                           paint_chroma);  // Bottom
  draw_line_yuva420p_thick(f, x1, y2, x1, y1, c, thickness,
                           paint_chroma);  // Left
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
    LOGW("the pixel format code [{}] is neither AV_PIX_FMT_YUV420P nor AV_PIX_FMT_YUVJ420P", f->format);
  }

  const int width  = f->width;
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
    draw_vertical_line_yuv420p(f, x1, left_start_y, left_end_y, color, thickness);
  }

  // Right edge (excluding corners already drawn)
  const int right_start_y = y1 + thickness;
  const int right_end_y = y2 - thickness;
  if (right_end_y >= right_start_y) {
    draw_vertical_line_yuv420p(f, x2 - thickness + 1, right_start_y, right_end_y, color, thickness);
  }
}

// Polygon outline with thickness
inline void draw_polygon_yuva420p_thick(AVFrame* f,
                                        const std::vector<cv::Point>& poly,
                                        const YUVAColor& c, int thickness = 2,
                                        bool paint_chroma = true) {
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

// scale YUV from full range → limited (for yuv420p)
inline YUVColor scale_for_limited(const YUVColor& c) noexcept {
    auto sy = [](uint8_t v) -> uint8_t { return static_cast<uint8_t>(16 + (v * 219 + 127) / 255); };
    auto sc = [](uint8_t v) -> uint8_t { return static_cast<uint8_t>(16 + (v * 224 + 127) / 255); };
    return { sy(c.y), sc(c.u), sc(c.v) };
}

inline void draw_rectangles_yuv420p_fast(AVFrame* f,
                                         const std::vector<cv::Rect>& rects,
                                         const YUVColor& color,
                                         int thickness)
{
    if (!f || thickness <= 0) return;
  if (const auto fmt = static_cast<AVPixelFormat>(f->format);
      fmt != AV_PIX_FMT_YUV420P && fmt != AV_PIX_FMT_YUVJ420P) {
    LOGW("the pixel format code [{}] is neither AV_PIX_FMT_YUV420P nor AV_PIX_FMT_YUVJ420P", f->format);
  }

    const int width  = f->width;
    const int height = f->height;

    uint8_t* Y = f->data[0];
    uint8_t* U = f->data[1];
    uint8_t* V = f->data[2];
    const int Y_stride = f->linesize[0];
    const int U_stride = f->linesize[1];
    const int V_stride = f->linesize[2];

    for (const auto& rect : rects) {
        int x1 = std::clamp(rect.x, 0, width  - 1);
        int y1 = std::clamp(rect.y, 0, height - 1);
        int x2 = std::clamp(rect.x + rect.width  - 1, 0, width  - 1);
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
                draw_horizontal_line_y_simd(U + (y1c + t) * U_stride, x1c, x2c, color.u);
                draw_horizontal_line_y_simd(V + (y1c + t) * V_stride, x1c, x2c, color.v);
            }
            if (y2c - t >= 0 && y2c - t < height / 2) {
                draw_horizontal_line_y_simd(U + (y2c - t) * U_stride, x1c, x2c, color.u);
                draw_horizontal_line_y_simd(V + (y2c - t) * V_stride, x1c, x2c, color.v);
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

// Creates the reusable polygon overlay frame.
AVFrame* create_polygon_overlay(int width, int height,
                                const std::vector<cv::Point>& vertices,
                                YUVAColor color) {
  AVFrame* overlay_frame = av_frame_alloc();
  if (!overlay_frame) return nullptr;

  overlay_frame->width = width;
  overlay_frame->height = height;
  overlay_frame->format = AV_PIX_FMT_YUVA420P;  // Format with Alpha channel

  if (av_frame_get_buffer(overlay_frame, 0) < 0) {
    av_frame_free(&overlay_frame);
    return nullptr;
  }

  // Make the frame fully transparent initially by setting Alpha (A) plane to 0
  // The Y, U, V values don't matter where it's transparent.
  memset(overlay_frame->data[3], 0, overlay_frame->linesize[3] * height);

  if (vertices.size() < 2) {
    return overlay_frame;  // Not enough points to draw
  }

  // Draw lines between consecutive vertices
  draw_polygon_yuva420p_thick(overlay_frame, vertices, color, 4, true);

  return overlay_frame;
}

int VideoOverlay::init(const std::vector<cv::Point>& polygon_vertices,
                       VideoContext cfg) {
  // Create the polygon overlay frame first
  polygon_overlay_frame.reset(create_polygon_overlay(
      cfg.width, cfg.height, polygon_vertices, {76, 84, 255, 255}/*Opaque Red in YUVA*/));
  if (!polygon_overlay_frame) {
    LOGE("Failed to create polygon overlay");
    return AVERROR(ENOMEM);
  }
  YUVColor color = {76, 84, 255}; // red
  bbox_color_ = static_cast<AVPixelFormat>(cfg.format) == AV_PIX_FMT_YUV420P ? scale_for_limited(color) : color;
  int width = cfg.width, height = cfg.height;
  auto [tnum, tden] = cfg.time_base.num ? cfg.time_base : AVRational{1, 1000};
  auto [num, den] = cfg.sample_aspect_ratio.num ? cfg.sample_aspect_ratio
                                            : AVRational{1, 1};

  const auto args_main = std::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}", width,
      height, av_get_pix_fmt_name(static_cast<AVPixelFormat>(cfg.format)), tnum, tden, num, den);

  const auto args_overlay = std::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect=1/1", width,
      height, av_get_pix_fmt_name(static_cast<AVPixelFormat>(polygon_overlay_frame->format)), tnum, tden);

  format = cfg.format;
  const std::string filter_descr = "[in][poly]overlay=format=auto[out]";

  filter_graph.reset(avfilter_graph_alloc());
  if (!filter_graph) throw std::runtime_error("Cannot allocate filter graph.");

  // Using smart pointers for inout to guarantee cleanup on failure
  AVFilterInOutPtr inputs{avfilter_inout_alloc()};
  AVFilterInOutPtr outputs{avfilter_inout_alloc()};
  AVFilterInOutPtr outputs_next{avfilter_inout_alloc()};
  if (!inputs || !outputs || !outputs_next)
    throw std::runtime_error("Cannot allocate filter in/out.");

  // 4. Create filter instances
  const AVFilter* buffersrc = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");

  int ret = avfilter_graph_create_filter(&buffersrc_ctx_main, buffersrc, "in",
                                         args_main.c_str(), nullptr,
                                         filter_graph.get());
  if (ret < 0) {
    LOGE_FFMPEG("Cannot create buffer source.", ret);
    return ret;
  }

  ret = avfilter_graph_create_filter(&buffersrc_ctx_overlay, buffersrc, "poly",
                                     args_overlay.c_str(), nullptr,
                                     filter_graph.get());
  if (ret < 0) {
    LOGE_FFMPEG("Cannot create buffer source for overlay.", ret);
    return ret;
  }

  ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                     nullptr, nullptr, filter_graph.get());
  if (ret < 0) {
    LOGE_FFMPEG("Cannot create buffer sink.", ret);
    return ret;
  }

  static const AVPixelFormat sink_pix_fmts[] = {format, AV_PIX_FMT_NONE};
  av_opt_set_int_list(buffersink_ctx, "pix_fmts", sink_pix_fmts,
                      AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

  // 5. Configure inputs and outputs for parsing
  outputs->name = av_strdup("in");
  outputs->filter_ctx = buffersrc_ctx_main;
  outputs->pad_idx = 0;
  outputs_next->name = av_strdup("poly");
  outputs_next->filter_ctx = buffersrc_ctx_overlay;
  outputs_next->pad_idx = 0;
  outputs_next->next = nullptr;
  outputs->next = outputs_next.release();

  inputs->name = av_strdup("out");
  inputs->filter_ctx = buffersink_ctx;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  // 6. Parse and configure the graph
  auto inputs_raw = inputs.release();
  auto outputs_raw = outputs.release();
  ret = avfilter_graph_parse_ptr(filter_graph.get(), filter_descr.c_str(),
                                 &inputs_raw, &outputs_raw, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("Cannot parse filter graph.", ret);
    return ret;
  }

  ret = avfilter_graph_config(filter_graph.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("Cannot configure filter graph.", ret);
    return ret;
  }
  return 0;
}

AVFramePtr VideoOverlay::draw_elements(AVFrame* in_frame,
                                       const std::vector<cv::Rect>& rects,
                                       int thickness) const {
  if (!in_frame) return nullptr;

  // Make sure the cloned frame is writable
  int ret = av_frame_make_writable(in_frame);
  if (ret < 0) {
    LOGE_FFMPEG("Cloned frame is not writable", ret);
    return nullptr;
  }

  // 2. Draw all rectangles directly onto the cloned frame's pixel data.
  // for (const auto& r : rects) {
  //   draw_rectangle_yuv420p(in_frame, r, {76, 84, 255}, thickness);
  // }
  draw_rectangles_yuv420p_fast(in_frame, rects, bbox_color_, thickness);

  // 3. Push the modified frame and the overlay into the static graph
  ret = av_buffersrc_add_frame_flags(buffersrc_ctx_main, in_frame,
                                     AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    LOGE_FFMPEG("Error adding main frame to buffer source", ret);
    return nullptr;
  }

  polygon_overlay_frame->pts = in_frame->pts;
  ret = av_buffersrc_add_frame_flags(buffersrc_ctx_overlay,
                                     polygon_overlay_frame.get(),
                                     AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    LOGE_FFMPEG("Error adding overlay frame to buffer source", ret);
    return nullptr;
  }

  // 4. Pull the final frame out
  AVFramePtr out_frame(av_frame_alloc());
  if (!out_frame) return nullptr;

  ret = av_buffersink_get_frame(buffersink_ctx, out_frame.get());
  if (ret < 0) {
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      LOGE_FFMPEG("Error getting frame from buffer sink", ret);
    }
    return nullptr;
  }
  av_frame_copy_props(out_frame.get(), in_frame);
  return out_frame;
}
}  // namespace avs_analyzer