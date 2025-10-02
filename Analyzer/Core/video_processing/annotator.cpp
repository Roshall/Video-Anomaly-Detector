//
// Created by lg on 10/2/25.
//

#include "annotator.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}



#include <opencv2/opencv.hpp>

namespace avs_analyzer {
// In video_overlay.cpp

// Represents a YUVA color. Red is {76, 84, 255}. Alpha=255 is fully opaque.
struct YUVAColor { uint8_t y, u, v, a; };

// Bounds-checked paint of one pixel in Y + A; optionally paint subsampled U/V
inline void paint_px_yuva420p(AVFrame* f, int x, int y,
                              const YUVAColor& c, bool paint_chroma = true)
{
  if ((unsigned)x >= (unsigned)f->width || (unsigned)y >= (unsigned)f->height) return;

  // Y and A planes are full resolution
  f->data[0][y * f->linesize[0] + x] = c.y; // Y
  f->data[3][y * f->linesize[3] + x] = c.a; // A

  if (paint_chroma) {
    // U/V are half resolution (4:2:0)
    int X = x >> 1, Y = y >> 1;
    // Guard in case of odd sizes/strides
    int maxUw = (f->width  + 1) >> 1;
    int maxUh = (f->height + 1) >> 1;
    if ((unsigned)X < (unsigned)maxUw && (unsigned)Y < (unsigned)maxUh) {
      f->data[1][Y * f->linesize[1] + X] = c.u; // U
      f->data[2][Y * f->linesize[2] + X] = c.v; // V
    }
  }
}

// Stamp a disk brush of radius r around (x0,y0)
inline void stamp_brush_disk(AVFrame* f, int x0, int y0, int r,
                             const YUVAColor& c, bool paint_chroma)
{
  if (r <= 0) { paint_px_yuva420p(f, x0, y0, c, paint_chroma); return; }
  int r2 = r * r;
  for (int oy = -r; oy <= r; ++oy) {
    int yy = y0 + oy;
    int oy2 = oy * oy;
    for (int ox = -r; ox <= r; ++ox) {
      if (ox*ox + oy2 > r2) continue;
      int xx = x0 + ox;
      paint_px_yuva420p(f, xx, yy, c, paint_chroma);
    }
  }
}

// Thickness-aware Bresenham line. thickness in pixels (>=1).
// Set paint_chroma=false for crisper thin lines (Y-only); true for colored lines.
inline void draw_line_yuva420p_thick(AVFrame* f,
                                     int x0, int y0, int x1, int y1,
                                     const YUVAColor& c,
                                     int thickness = 2,
                                     bool paint_chroma = true)
{
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
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Polygon outline with thickness
inline void draw_polygon_yuva420p_thick(AVFrame* f,
                                        const std::vector<cv::Point>& poly,
                                        const YUVAColor& c,
                                        int thickness = 2,
                                        bool paint_chroma = true)
{
  if (poly.size() < 2) return;
  for (size_t i = 0; i < poly.size(); ++i) {
    const auto& a = poly[i];
    const auto& b = poly[(i + 1) % poly.size()];
    draw_line_yuva420p_thick(f, a.x, a.y, b.x, b.y, c, thickness, paint_chroma);
  }
}

// Creates the reusable polygon overlay frame.
AVFrame* create_polygon_overlay(int width, int height, const std::vector<cv::Point>& vertices, YUVAColor color) {
  AVFrame* overlay_frame = av_frame_alloc();
  if (!overlay_frame) return nullptr;

  overlay_frame->width = width;
  overlay_frame->height = height;
  overlay_frame->format = AV_PIX_FMT_YUVA420P; // Format with Alpha channel

  if (av_frame_get_buffer(overlay_frame, 32) < 0) {
    av_frame_free(&overlay_frame);
    return nullptr;
  }

  // Make the frame fully transparent initially by setting Alpha (A) plane to 0
  // The Y, U, V values don't matter where it's transparent.
  memset(overlay_frame->data[3], 0, overlay_frame->linesize[3] * height);

  if (vertices.size() < 2) {
    return overlay_frame; // Not enough points to draw
  }

  // Draw lines between consecutive vertices
  draw_polygon_yuva420p_thick(overlay_frame, vertices, color, 4, false);

  return overlay_frame;
}

void VideoOverlay:: cleanup() {
  avfilter_graph_free(&filter_graph);
  buffersrc_ctx_main = nullptr;
  buffersink_ctx = nullptr;
}

VideoOverlay::~VideoOverlay() {
  cleanup();
}
int VideoOverlay::init(const std::vector<cv::Point>& polygon_vertices,
                       AVFrame* frame) {
  cleanup();

  // 1. Create the polygon overlay frame first
  YUVAColor red = {76, 84, 255, 255}; // Opaque Red in YUVA
  polygon_overlay_frame = create_polygon_overlay(frame->width, frame->height, polygon_vertices, red);
  if (!polygon_overlay_frame) {
    std::cerr << "Failed to create polygon overlay" << std::endl;
    return AVERROR(ENOMEM);
  }
  int width = frame->width, height = frame->height;
  auto tb = frame->time_base.num ? frame->time_base : AVRational{1,1000};
  auto sar = frame->sample_aspect_ratio.num ? frame->sample_aspect_ratio: AVRational{1,1};

  args_main = std::format("video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}",
                                   width, height, frame->format,
                                   tb.num, tb.den, sar.num, sar.den);

  args_overlay = std::format("video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect=1/1",
                                      width, height, polygon_overlay_frame->format, tb.num, tb.den);

  format = (AVPixelFormat)frame->format;
  last_bbox_str = "";
  return 0;
}

AVFramePtr VideoOverlay::draw_elements(AVFrame* in_frame,
                                       const std::vector<cv::Rect>& rects,
                                       int thickness) {
  if (!in_frame) return nullptr;

  // 1. Build the dynamic drawbox filter string
  std::stringstream drawbox_ss;
  for (size_t i = 0; i < rects.size(); ++i) {
    const auto& r = rects[i];
    drawbox_ss << "drawbox=x=" << r.x << ":y=" << r.y
               << ":w=" << r.width << ":h=" << r.height
               << ":color=red:t=" << thickness;
    if (i < rects.size() - 1) {
      drawbox_ss << ",";
    }
  }
  std::string drawbox_str = drawbox_ss.str();

  // 2. Construct the full filter chain
  std::string current_filter_descr;
  if (!drawbox_str.empty()) {
    current_filter_descr = "[in][poly]overlay=format=auto:shortest=1:eof_action=repeat[bg];[bg]" + drawbox_str + "[out]";
  } else {
    current_filter_descr = "[in][poly]overlay=format=auto[out]";
  }

  if (last_bbox_str != drawbox_str) {
    if (filter_graph) avfilter_graph_free(&filter_graph);
    filter_graph = avfilter_graph_alloc();

    // 2) Create sources and sink FIRST (so filter_ctx pointers are valid)
    const AVFilter* buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");


    int ret = avfilter_graph_create_filter(&buffersrc_ctx_main, buffersrc, "in",
                                           args_main.c_str(), nullptr, filter_graph);
    if (ret < 0) return nullptr;


    ret = avfilter_graph_create_filter(&buffersrc_ctx_overlay, buffersrc, "poly",
                                       args_overlay.c_str(), nullptr, filter_graph);
    if (ret < 0) return nullptr;

    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       nullptr, nullptr, filter_graph);
    if (ret < 0) return nullptr;

    // (Optional) Force sink format to match your encoder/input expectations
    static const AVPixelFormat sink_pix_fmts[] = { format, AV_PIX_FMT_NONE };
    ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", sink_pix_fmts, AV_PIX_FMT_NONE,
                              AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) return nullptr;

    // 3) Build the in/out maps (pad_idx=0, names match labels in the string)
    AVFilterInOut* inputs  = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) return nullptr;

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx_main;
    outputs->pad_idx    = 0;
    outputs->next       = avfilter_inout_alloc();
    outputs->next->name       = av_strdup("poly");
    outputs->next->filter_ctx = buffersrc_ctx_overlay;
    outputs->next->pad_idx    = 0;
    outputs->next->next       = nullptr;

    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = buffersink_ctx;
    inputs->pad_idx     = 0;
    inputs->next        = nullptr;

    // 4) Parse and config
    ret = avfilter_graph_parse_ptr(filter_graph, current_filter_descr.c_str(), &inputs, &outputs, nullptr);
    if (ret < 0) {
      // log filter_descr to debug
      return nullptr;
    }

    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
      log_error("can't create configure graph", ret);
      return nullptr;
    }

    std::swap(last_bbox_str, drawbox_str);
  }

  // 4. Push frames into the graph
  int ret = av_buffersrc_add_frame_flags(buffersrc_ctx_main, in_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    log_error("can't add frame flags to buffer src[1]", ret);
    return nullptr;
  }
  polygon_overlay_frame->pts = in_frame->pts;
  ret = av_buffersrc_add_frame_flags(buffersrc_ctx_overlay, polygon_overlay_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    log_error("can't add frame flags to buffer src[2]", ret);
    return nullptr;
  }
  // 5. Pull the final frame out
  auto frame = av_frame_alloc();
  ret = av_buffersink_get_frame(buffersink_ctx, frame);
  if (ret < 0){
    log_error("can't get frame", ret);
    av_frame_free(&frame);
    return nullptr;
  }
  return AVFramePtr{frame};
}

}