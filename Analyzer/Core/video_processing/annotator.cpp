//
// Created by lg on 10/2/25.
//

#include "annotator.h"
#include <spdlog/fmt/fmt.h>

#include <format>

#include "painter.h"
#include "painter_shapes.h"

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
  draw_polygon_yuva420p(overlay_frame, vertices, color, 4, true);

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

  const auto args_main = fmt::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}", width,
      height, av_get_pix_fmt_name(static_cast<AVPixelFormat>(cfg.format)), tnum, tden, num, den);

  const auto args_overlay = fmt::format(
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
  draw_rectangles_yuv420p_batch(in_frame, rects, bbox_color_, thickness);

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