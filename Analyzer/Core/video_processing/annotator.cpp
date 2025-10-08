//
// Created by lg on 10/2/25.
//

#include "annotator.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <opencv2/opencv.hpp>

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

// Represents a YUV color. Red is {76, 84, 255}.
struct YUVColor {
  uint8_t y, u, v;
};

/**
 * @brief Paints a single pixel on a YUV420p frame.
 *
 * This function handles the chroma subsampling (4:2:0), where one U/V sample
 * corresponds to a 2x2 block of Y samples.
 */
inline void paint_px_yuv420p(AVFrame* f, int x, int y, const YUVColor& c) {
  // Bounds check for the luma (Y) plane
  if ((unsigned)x >= (unsigned)f->width || (unsigned)y >= (unsigned)f->height) {
    return;
  }

  // 1. Set the Y value (full resolution)
  f->data[0][y * f->linesize[0] + x] = c.y;

  // 2. Set the U and V values (half resolution)
  // A 2x2 block of pixels shares a single U and V sample.
  int chroma_x = x / 2;
  int chroma_y = y / 2;
  f->data[1][chroma_y * f->linesize[1] + chroma_x] = c.u;
  f->data[2][chroma_y * f->linesize[2] + chroma_x] = c.v;
}

inline void stamp_brush_disk_yuv420p(AVFrame* f, int x0, int y0, int r,
                                     const YUVColor& c) {
  if (r <= 0) {
    paint_px_yuv420p(f, x0, y0, c);
    return;
  }
  int r2 = r * r;
  for (int oy = -r; oy <= r; ++oy) {
    int oy2 = oy * oy;
    for (int ox = -r; ox <= r; ++ox) {
      if (ox * ox + oy2 > r2) continue;
      paint_px_yuv420p(f, x0 + ox, y0 + oy, c);
    }
  }
}

inline void draw_line_yuv420p_thick(AVFrame* f, int x0, int y0, int x1, int y1,
                                    const YUVColor& c, int thickness) {
  if (f->format != AV_PIX_FMT_YUV420P) return;

  int r = std::max(0, thickness / 2);
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  while (true) {
    stamp_brush_disk_yuv420p(f, x0, y0, r, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
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

inline void draw_rectangle_yuv420p_thick(AVFrame* f, const cv::Rect& r,
                                         const YUVColor& c, int thickness) {
  int x1 = r.x, y1 = r.y;
  int x2 = r.x + r.width, y2 = r.y + r.height;

  draw_line_yuv420p_thick(f, x1, y1, x2, y1, c, thickness);  // Top
  draw_line_yuv420p_thick(f, x2, y1, x2, y2, c, thickness);  // Right
  draw_line_yuv420p_thick(f, x2, y2, x1, y2, c, thickness);  // Bottom
  draw_line_yuv420p_thick(f, x1, y2, x1, y1, c, thickness);  // Left
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
  draw_polygon_yuva420p_thick(overlay_frame, vertices, color, 4, false);

  return overlay_frame;
}

int VideoOverlay::init(const std::vector<cv::Point>& polygon_vertices,
                       AVFrame* frame) {
  // 1. Create the polygon overlay frame first
  YUVAColor red = {76, 84, 255, 255};  // Opaque Red in YUVA
  polygon_overlay_frame.reset(create_polygon_overlay(
      frame->width, frame->height, polygon_vertices, red));
  if (!polygon_overlay_frame) {
    std::cerr << "Failed to create polygon overlay" << std::endl;
    return AVERROR(ENOMEM);
  }
  int width = frame->width, height = frame->height;
  auto tb = frame->time_base.num ? frame->time_base : AVRational{1, 1000};
  auto sar = frame->sample_aspect_ratio.num ? frame->sample_aspect_ratio
                                            : AVRational{1, 1};

  auto args_main = std::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}", width,
      height, frame->format, tb.num, tb.den, sar.num, sar.den);

  auto args_overlay = std::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect=1/1", width,
      height, polygon_overlay_frame->format, tb.num, tb.den);

  format = (AVPixelFormat)frame->format;
  const std::string filter_descr = "[in][poly]overlay=format=auto[out]";

  filter_graph.reset(avfilter_graph_alloc());
  if (!filter_graph) throw std::runtime_error("Cannot allocate filter graph.");

  // Using smart pointers for inout to guarantee cleanup on failure
  AVFilterInOutPtr inputs{avfilter_inout_alloc()};
  AVFilterInOutPtr outputs{avfilter_inout_alloc()};
  if (!inputs || !outputs)
    throw std::runtime_error("Cannot allocate filter in/out.");

  // 4. Create filter instances
  const AVFilter* buffersrc = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");

  int ret = 0;
  ret = avfilter_graph_create_filter(&buffersrc_ctx_main, buffersrc, "in",
                                     args_main.c_str(), nullptr,
                                     filter_graph.get());
  if (ret < 0)
    throw std::runtime_error("Cannot create buffer source for main.");

  ret = avfilter_graph_create_filter(&buffersrc_ctx_overlay, buffersrc, "poly",
                                     args_overlay.c_str(), nullptr,
                                     filter_graph.get());
  if (ret < 0)
    throw std::runtime_error("Cannot create buffer source for overlay.");

  ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                     nullptr, nullptr, filter_graph.get());
  if (ret < 0) throw std::runtime_error("Cannot create buffer sink.");

  static const AVPixelFormat sink_pix_fmts[] = {format, AV_PIX_FMT_NONE};
  av_opt_set_int_list(buffersink_ctx, "pix_fmts", sink_pix_fmts,
                      AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

  // 5. Configure inputs and outputs for parsing
  outputs->name = av_strdup("in");
  outputs->filter_ctx = buffersrc_ctx_main;
  outputs->pad_idx = 0;
  outputs->next = avfilter_inout_alloc();
  outputs->next->name = av_strdup("poly");
  outputs->next->filter_ctx = buffersrc_ctx_overlay;
  outputs->next->pad_idx = 0;
  outputs->next->next = nullptr;

  inputs->name = av_strdup("out");
  inputs->filter_ctx = buffersink_ctx;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  // 6. Parse and configure the graph
  auto inputs_raw = inputs.release();
  auto outputs_raw = outputs.release();
  ret = avfilter_graph_parse_ptr(filter_graph.get(), filter_descr.c_str(),
                                 &inputs_raw, &outputs_raw, nullptr);
  if (ret < 0) throw std::runtime_error("Cannot parse filter graph.");

  ret = avfilter_graph_config(filter_graph.get(), nullptr);
  if (ret < 0) throw std::runtime_error("Cannot configure filter graph.");
  return 0;
}

AVFramePtr VideoOverlay::draw_elements(AVFrame* in_frame,
                                       const std::vector<cv::Rect>& rects,
                                       int thickness) {
  if (!in_frame) return nullptr;

  // Make sure the cloned frame is writable
  int ret = av_frame_make_writable(in_frame);
  if (ret < 0) {
    log_error("Cloned frame is not writable", ret);
    return nullptr;
  }

  // 2. Draw all rectangles directly onto the cloned frame's pixel data.
  const YUVColor rect_color = {76, 84, 255};  // Red
  for (const auto& r : rects) {
    // NOTE: This assumes the input format is YUVA420P. A robust implementation
    // would need to handle different pixel formats.
    draw_rectangle_yuv420p_thick(in_frame, r, rect_color, thickness);
  }

  // 3. Push the modified frame and the overlay into the static graph
  ret = av_buffersrc_add_frame_flags(buffersrc_ctx_main, in_frame,
                                     AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    log_error("Error adding main frame to buffer source", ret);
    return nullptr;
  }

  polygon_overlay_frame->pts = in_frame->pts;
  ret = av_buffersrc_add_frame_flags(buffersrc_ctx_overlay,
                                     polygon_overlay_frame.get(),
                                     AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    log_error("Error adding overlay frame to buffer source", ret);
    return nullptr;
  }

  // 4. Pull the final frame out
  AVFramePtr out_frame(av_frame_alloc());
  if (!out_frame) return nullptr;

  ret = av_buffersink_get_frame(buffersink_ctx, out_frame.get());
  if (ret < 0) {
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      log_error("Error getting frame from buffer sink", ret);
    }
    return nullptr;
  }
  return out_frame;
}
}  // namespace avs_analyzer