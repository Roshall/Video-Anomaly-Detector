#include "dynamic_annotator.h"
#include "painter.h"
#include "painter_shapes.h"
#include "painter_optimized.h"
#include <algorithm>
#include <stdexcept>
#include <format>

#include "painter_text.h"
#include "task.h"
#include <Utils/log.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

namespace avs_analyzer {

YUVColor Task2Color(const Task task) {
  switch (task) {
    case Task::kPhonePlaying:
        return {152, 165, 83};
      case Task::kSleepWork:
        return {107, 133, 62};
      case Task::kWandering:
        return {199, 55, 145};
      case Task::kBreakIn:
        return {91, 173, 74};
      case Task::kWallClimbing:
        return {118, 69, 187};
      case Task::kBunkOff:
        return {146, 136, 161};
      default:
        return {255, 128, 128};
  }
}
YUVAColor Task2ColorAlpha(const Task task) {
  const auto [y, u, v] = Task2Color(task);
  return {y, u, v, 220};
}

int DynamicAnnotator::init(const VideoContext& video_ctx,
                           const LegendConfig& legend_config) {
  video_ctx_ = video_ctx;
  legend_config_ = legend_config;

  // Clear any existing legend overlay
  legend_overlay_frame_.reset();

  return 0;
}

void DynamicAnnotator::update_legend(const std::vector<Task>& tasks) {
  if (tasks.empty()) {
    legend_overlay_frame_.reset();
    return;
  }

  // Build labels and colors from Task list
  std::vector<std::u8string> labels_u8;
  labels_u8.reserve(tasks.size());
  std::vector<YUVAColor> colors;
  colors.reserve(tasks.size());

  for (const auto& t : tasks) {
    labels_u8.emplace_back(toChinese(t));
    colors.push_back(Task2ColorAlpha(t));
  }

  // Get accurate legend size
  const auto [legend_width, legend_height] = estimate_legend_size_accurate(
      legend_config_.font_path,
      legend_config_.font_size,
      legend_config_.handle_size,
      legend_config_.row_spacing,
      labels_u8,
      legend_config_.text_padding,
      legend_config_.frame_padding
      );

  // Calculate legend position
  legend_layout_ = estimate_legend_layout(
      legend_config_.font_size,
      legend_config_.handle_size,
      legend_config_.row_spacing,
      labels_u8,
      legend_config_.text_padding,
      legend_config_.frame_padding,
      video_ctx_.width,
      video_ctx_.height,
      legend_config_.position,
      legend_config_.margin_x,
      legend_config_.margin_y
      );

  // Create legend overlay buffer with exact size (YUVA420P for FFmpeg overlay filter)
  try {
    legend_overlay_frame_.reset(create_blank_yuva420p_optimized(legend_width, legend_height));
    // Draw legend in the buffer
    draw_legend_yuva420p_optimized(
        legend_overlay_frame_.get(),
        legend_config_.font_path,
        {legend_config_.frame_padding, legend_config_.frame_padding + legend_config_.font_size},
        legend_config_.font_size,
        legend_config_.handle_size,
        legend_config_.row_spacing,
        colors,
        labels_u8,
        {legend_config_.text_padding, legend_config_.frame_padding, legend_config_.frame_thickness, true, legend_config_.frame_color},
        true
    );
    // Re-init filter graph
    init_filter_graph();
  } catch (const std::exception& e) {
    legend_overlay_frame_.reset();
    LOGE("Failed to create legend overlay frame: {}", e.what());
  }
}

int DynamicAnnotator::init_filter_graph() {
  if (!legend_overlay_frame_) {
    return -1;
  }

  const int width = video_ctx_.width;
  const int height = video_ctx_.height;
  auto [tnum, tden] = video_ctx_.time_base.num
                        ? video_ctx_.time_base
                        : AVRational{1, 1000};
  auto [num, den] = video_ctx_.sample_aspect_ratio.num
                      ? video_ctx_.sample_aspect_ratio
                      : AVRational{1, 1};

  const auto args_main = std::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}",
      width, height,
      av_get_pix_fmt_name(video_ctx_.format),
      tnum, tden, num, den);

    // Overlay filter expects a YUVA420P overlay frame of the same video size (full frame)
    const auto args_overlay = std::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect=1/1",
      width, height,
      av_get_pix_fmt_name(AV_PIX_FMT_YUVA420P),
      tnum, tden);

  // Overlay filter positioned at legend_layout_ coordinates
  const std::string filter_descr = std::format(
      "[in][legend]overlay=x={}:y={}:format=auto[out]",
      legend_layout_.pos_x, legend_layout_.pos_y);

  filter_graph_.reset(avfilter_graph_alloc());
  if (!filter_graph_) {
    LOGE("Failed to allocate filter graph");
    return -1;
  }

  AVFilterInOutPtr inputs{avfilter_inout_alloc()};
  AVFilterInOutPtr outputs{avfilter_inout_alloc()};
  AVFilterInOutPtr outputs_next{avfilter_inout_alloc()};
  if (!inputs || !outputs || !outputs_next) {
    LOGE("Failed to allocate filter in/out");
    return -1;
  }

  const AVFilter* buffersrc = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");

  int ret = avfilter_graph_create_filter(&buffersrc_ctx_main_, buffersrc, "in",
                                         args_main.c_str(), nullptr,
                                         filter_graph_.get());
  if (ret < 0) {
    LOGE_FFMPEG("Cannot create buffer source.", ret);
    return ret;
  }

  ret = avfilter_graph_create_filter(&buffersrc_ctx_overlay_, buffersrc,
                                     "legend",
                                     args_overlay.c_str(), nullptr,
                                     filter_graph_.get());
  if (ret < 0) {
    LOGE_FFMPEG("Cannot create buffer source for overlay.", ret);
    return ret;
  }

  ret = avfilter_graph_create_filter(&buffersink_ctx_, buffersink, "out",
                                     nullptr, nullptr, filter_graph_.get());
  if (ret < 0) {
    LOGE_FFMPEG("Cannot create buffer sink.", ret);
    return ret;
  }

  static const AVPixelFormat sink_pix_fmts[] = {
      video_ctx_.format,
      AV_PIX_FMT_NONE
  };
  av_opt_set_int_list(buffersink_ctx_, "pix_fmts", sink_pix_fmts,
                      AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

  // Configure inputs and outputs
  outputs->name = av_strdup("in");
  outputs->filter_ctx = buffersrc_ctx_main_;
  outputs->pad_idx = 0;
  outputs_next->name = av_strdup("legend");
  outputs_next->filter_ctx = buffersrc_ctx_overlay_;
  outputs_next->pad_idx = 0;
  outputs_next->next = nullptr;
  outputs->next = outputs_next.release();

  inputs->name = av_strdup("out");
  inputs->filter_ctx = buffersink_ctx_;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  auto inputs_raw = inputs.release();
  auto outputs_raw = outputs.release();
  ret = avfilter_graph_parse_ptr(filter_graph_.get(), filter_descr.c_str(),
                                 &inputs_raw, &outputs_raw, nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("Cannot parse filter graph.", ret);
    return ret;
  }

  ret = avfilter_graph_config(filter_graph_.get(), nullptr);
  if (ret < 0) {
    LOGE_FFMPEG("Cannot configure filter graph.", ret);
    return ret;
  }

  return 0;
}

AVFramePtr DynamicAnnotator::draw_elements(AVFrame* in_frame,
                                           const std::vector<ROI>& rois,
                                           const std::vector<DetectionBox>&
                                           detections) const {
  if (!in_frame) return nullptr;

  // Make sure the frame is writable
  int ret = av_frame_make_writable(in_frame);
  if (ret < 0) {
    LOGE_FFMPEG("Failed to make frame writable.", ret);
    return nullptr;
  }

  // Draw detection bounding boxes directly on YUV420P frame (opaque boxes)
  for (const auto& detection : detections) {
    YUVColor color = detection.color;
    if (static_cast<AVPixelFormat>(video_ctx_.format) == AV_PIX_FMT_YUV420P) {
      color = scale_for_limited(color);
    }
    draw_rectangle_yuv420p_optimized(in_frame, detection.bbox, color,
                                     detection.thickness);
  }

  // Prepare a full-frame YUVA overlay for ROIs + legend composition
  AVFramePtr rois_overlay_frame(create_blank_yuva420p_optimized(video_ctx_.width, video_ctx_.height));
  if (!rois_overlay_frame) {
    // If overlay cannot be created, return input frame with rectangles only
    AVFramePtr result(av_frame_clone(in_frame));
    return result;
  }

  // Initialize overlay frame alpha to 0 (fully transparent)
  for (int y = 0; y < rois_overlay_frame->height; ++y) {
    memset(rois_overlay_frame->data[3] + y * rois_overlay_frame->linesize[3], 0, rois_overlay_frame->width);
  }

  // Draw ROI quadrilaterals into the overlay frame with YUVAColor (alpha)
  BatchPainter roi_painter(rois_overlay_frame.get());
  const std::vector<YUVAColor> default_roi_colors = {
      {255, 128, 128, 200}, // Semi-transparent white
      {76, 84, 255, 200}, // Semi-transparent blue
      {149, 43, 21, 200}, // Semi-transparent green
      {225, 211, 114, 200}, // Semi-transparent yellow
      {174, 128, 128, 200}, // Semi-transparent gray
  };
  for (size_t i = 0; i < rois.size(); ++i) {
    const auto& roi = rois[i];
    YUVAColor color = roi.color;
    if (color.a == 0) { // if not set, use default
      color = default_roi_colors[i % default_roi_colors.size()];
    }
    roi_painter.add_quadrilateral(roi.quadrilateral, color, roi.thickness);
  }
  roi_painter.execute();
  const std::vector<YUVColor> roi_colors = {
      {255, 128, 128}, // White
      {76, 84, 255}, // Blue
      {149, 43, 21}, // Green
      {225, 211, 114}, // Yellow
      {174, 128, 128}, // Gray
  };

  for (size_t i = 0; i < rois.size(); ++i) {
    const auto& roi = rois[i];
    YUVColor color = (i < roi_colors.size()) ? roi_colors[i] : roi_colors[0];

    // Scale for limited range if needed
    if (static_cast<AVPixelFormat>(video_ctx_.format) == AV_PIX_FMT_YUV420P) {
      color = scale_for_limited(color);
    }

    draw_polygon_yuv420p_optimized(in_frame, roi.quadrilateral, color,
                                   roi.thickness);
  }

  // Overlay legend on overlay frame (composite legend into this full-sized overlay)
  if (legend_overlay_frame_) {
    overlay_yuva420p_region(rois_overlay_frame.get(), legend_overlay_frame_.get(),
                           legend_layout_.pos_x, legend_layout_.pos_y,
                           legend_overlay_frame_->width, legend_overlay_frame_->height);
  }

  // If no legend, return the frame as is
  if (!legend_overlay_frame_ || !filter_graph_) {
    AVFramePtr result(av_frame_clone(in_frame));
    return result;
  }

  // Push frames through overlay filter: main + overlay (full-frame YUVA)
  ret = av_buffersrc_add_frame_flags(buffersrc_ctx_main_, in_frame,
                                     AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    LOGE_FFMPEG("Cannot add frame to buffer source.", ret);
    return nullptr;
  }

  rois_overlay_frame->pts = in_frame->pts;
  ret = av_buffersrc_add_frame_flags(buffersrc_ctx_overlay_,
                                     rois_overlay_frame.get(),
                                     AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    LOGE_FFMPEG("Cannot add legend overlay frame to buffer source.", ret);
    return nullptr;
  }

  // Pull the final frame out
  AVFramePtr out_frame(av_frame_alloc());
  if (!out_frame) {
    LOGE("Failed to allocate output frame.");
    return nullptr;
  }

  ret = av_buffersink_get_frame(buffersink_ctx_, out_frame.get());
  if (ret < 0) {
    LOGE_FFMPEG("Cannot get frame from buffer sink.", ret);
    return nullptr;
  }

  av_frame_copy_props(out_frame.get(), in_frame);
  return out_frame;
}

// Optimized overlay function with SIMD
void overlay_yuva420p_region(AVFrame* dst, const AVFrame* overlay,
                             int dst_x, int dst_y,
                             int region_w, int region_h) {
  if (!dst || !overlay) {
    throw std::invalid_argument("overlay_yuva420p_region: null frame");
  }

  if (dst->format != AV_PIX_FMT_YUVA420P || overlay->format !=
      AV_PIX_FMT_YUVA420P) {
    throw std::runtime_error(
        "overlay_yuva420p_region: both frames must be YUVA420P");
  }

  const int ow = region_w > 0
                   ? std::min(region_w, overlay->width)
                   : overlay->width;
  const int oh = region_h > 0
                   ? std::min(region_h, overlay->height)
                   : overlay->height;
  if (ow <= 0 || oh <= 0) return;

  const int W = dst->width;
  const int H = dst->height;

  // ---- Y plane with SIMD optimization ----
#pragma omp simd
  for (int y = 0; y < oh; ++y) {
    int dy = dst_y + y;
    if (static_cast<unsigned>(dy) >= static_cast<unsigned>(H)) continue;

    const uint8_t* __restrict Yo = overlay->data[0] + y * overlay->linesize[0];
    const uint8_t* __restrict Ao = overlay->data[3] + y * overlay->linesize[3];
    uint8_t* __restrict Yd = dst->data[0] + dy * dst->linesize[0];

    // Compute src x-range for this row (avoid per-pixel bound checks)
    const int src_x0 = std::max(0, -dst_x);
    const int src_x1 = std::min(ow, W - dst_x);
    const uint8_t* __restrict Yo_row = Yo + src_x0;
    const uint8_t* __restrict Ao_row = Ao + src_x0;
    uint8_t* __restrict Yd_row = Yd + dst_x + src_x0;

    #pragma omp simd
    for (int sx = src_x0; sx < src_x1; ++sx) {
      const uint8_t a = *Ao_row++;
      if (a == 0) { Yd_row++; Yo_row++; continue; }
      if (a == 255) { *Yd_row = *Yo_row; Yd_row++; Yo_row++; continue; }
      const int src_val = static_cast<int>(*Yo_row);
      const int dst_val = static_cast<int>(*Yd_row);
      const int diff = src_val - dst_val;
      *Yd_row = static_cast<uint8_t>(dst_val + (diff * a) / 255);
      Yd_row++; Yo_row++;
    }
  }

  // ---- U/V planes with SIMD optimization ----
  const int ow2 = (ow + 1) >> 1;
  const int oh2 = (oh + 1) >> 1;

  for (int c = 1; c <= 2; ++c) {
    const int Wc = (W + 1) >> 1;
    const int Hc = (H + 1) >> 1;

#pragma omp simd
    for (int y = 0; y < oh2; ++y) {
      int dy = (dst_y >> 1) + y;
      if (static_cast<unsigned>(dy) >= static_cast<unsigned>(Hc)) continue;

    const uint8_t* __restrict Co = overlay->data[c] + y * overlay->linesize[c];
    uint8_t* __restrict Cd = dst->data[c] + dy * dst->linesize[c];

      // Sample alpha from top-left of each 2x2 block
      const uint8_t* __restrict Ao =
          overlay->data[3] + (y * 2) * overlay->linesize[3];

      // Source x-range for chroma row
      const int src_cx0 = std::max(0, - (dst_x >> 1));
      const int src_cx1 = std::min(ow2, Wc - (dst_x >> 1));
      const uint8_t* __restrict Co_row = Co + src_cx0;
      const uint8_t* __restrict Ao_row_c = Ao + (src_cx0 * 2);
      uint8_t* __restrict Cd_row = Cd + (dst_x >> 1) + src_cx0;

      #pragma omp simd
      for (int cx = src_cx0; cx < src_cx1; ++cx) {
        const uint8_t a = *Ao_row_c; Ao_row_c += 2;
        if (a == 0) { Cd_row++; Co_row++; continue; }
        if (a == 255) { *Cd_row = *Co_row; Cd_row++; Co_row++; continue; }
        const int src_val = static_cast<int>(*Co_row);
        const int dst_val = static_cast<int>(*Cd_row);
        const int diff = src_val - dst_val;
        *Cd_row = static_cast<uint8_t>(dst_val + (diff * a) / 255);
        Cd_row++; Co_row++;
      }
    }
  }
}
} // namespace avs_analyzer