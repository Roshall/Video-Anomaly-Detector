#include "dynamic_annotator.h"
#include "painter.h"
#include "painter_shapes.h"
#include "painter_optimized.h"
#include <algorithm>
#include <stdexcept>
#include <format>

#include "painter_text.h"

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
int DynamicAnnotator::init(const VideoContext& video_ctx,
                           const LegendConfig& legend_config) {
  video_ctx_ = video_ctx;
  legend_config_ = legend_config;

  // Clear any existing data
  roi_labels_.clear();
  detection_labels_.clear();
  legend_colors_.clear();
  legend_labels_utf8_.clear();
  legend_overlay_frame_.reset();

  // Scale color for limited range if needed
  YUVColor color = {76, 84, 255}; // red
  bbox_color_ = static_cast<AVPixelFormat>(video_ctx.format) ==
                AV_PIX_FMT_YUV420P
                  ? scale_for_limited(color)
                  : color;

  return 0;
}

void DynamicAnnotator::set_labels(const std::vector<std::string>& roi_labels,
                                  const std::vector<std::string>&
                                  detection_labels) {
  roi_labels_ = roi_labels;
  detection_labels_ = detection_labels;

  prepare_legend_data();
  update_legend();

  // Initialize filter graph with the legend overlay
  if (legend_overlay_frame_) {
    init_filter_graph();
  }
}

void DynamicAnnotator::prepare_legend_data() {
  legend_colors_.clear();
  legend_labels_utf8_.clear();

  // Add ROI labels with different colors
  const std::vector<YUVAColor> roi_colors = {
      {255, 128, 128, 255}, // White
      {76, 84, 255, 255}, // Blue
      {149, 43, 21, 255}, // Green
      {225, 211, 114, 255}, // Yellow
      {174, 128, 128, 255}, // Gray
  };

  for (size_t i = 0; i < roi_labels_.size(); ++i) {
    legend_colors_.push_back(roi_colors[i % roi_colors.size()]);
    legend_labels_utf8_.emplace_back(
        reinterpret_cast<const char8_t*>(roi_labels_[i].c_str()));
  }

  // Add detection box labels with different colors
  const std::vector<YUVAColor> detection_colors = {
      {255, 128, 128, 255}, // White
      {255, 84, 255, 255}, // Red
      {255, 211, 114, 255}, // Orange
      {76, 128, 255, 255}, // Cyan
      {149, 128, 21, 255}, // Purple
  };

  for (size_t i = 0; i < detection_labels_.size(); ++i) {
    legend_colors_.push_back(detection_colors[i % detection_colors.size()]);
    legend_labels_utf8_.emplace_back(
        reinterpret_cast<const char8_t*>(detection_labels_[i].c_str()));
  }
}

void DynamicAnnotator::update_legend() {
  if (legend_colors_.empty() || legend_labels_utf8_.empty()) {
    legend_overlay_frame_.reset();
    return;
  }

  // Get accurate legend size
  std::vector<std::string> all_labels;
  all_labels.reserve(roi_labels_.size() + detection_labels_.size());
  all_labels.insert(all_labels.end(), roi_labels_.begin(), roi_labels_.end());
  all_labels.insert(all_labels.end(), detection_labels_.begin(),
                    detection_labels_.end());

  const auto [legend_width, legend_height] = estimate_legend_size_accurate(
      legend_config_.font_path,
      legend_config_.font_size,
      legend_config_.handle_size,
      legend_config_.row_spacing,
      all_labels,
      legend_config_.text_padding,
      legend_config_.frame_padding
      );

  // Calculate legend position
  legend_layout_ = estimate_legend_layout(
      legend_config_.font_path,
      legend_config_.font_size,
      legend_config_.handle_size,
      legend_config_.row_spacing,
      all_labels,
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
    legend_overlay_frame_.reset(
        create_blank_yuva420p_optimized(legend_width, legend_height));

    // Draw legend in the buffer
    draw_legend_yuva420p_optimized(
        legend_overlay_frame_.get(),
        legend_config_.font_path,
        {legend_config_.frame_padding,
         legend_config_.frame_padding + legend_config_.font_size},
        // Origin within buffer
        legend_config_.font_size,
        legend_config_.handle_size,
        legend_config_.row_spacing,
        legend_colors_,
        legend_labels_utf8_, {
            legend_config_.text_padding,
            legend_config_.frame_padding,
            legend_config_.frame_thickness,
            true, // draw_frame
            legend_config_.frame_color}
        );
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
      av_get_pix_fmt_name(static_cast<AVPixelFormat>(video_ctx_.format)),
      tnum, tden, num, den);

  const auto args_overlay = std::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect=1/1",
      legend_overlay_frame_->width, legend_overlay_frame_->height,
      av_get_pix_fmt_name(
          static_cast<AVPixelFormat>(legend_overlay_frame_->format)),
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
      static_cast<AVPixelFormat>(video_ctx_.format),
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

  // Draw ROI quadrilaterals directly on YUV420P frame
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

  // Draw detection bounding boxes
  for (const auto& detection : detections) {
    YUVColor color = detection.color;
    if (static_cast<AVPixelFormat>(video_ctx_.format) == AV_PIX_FMT_YUV420P) {
      color = scale_for_limited(color);
    }
    draw_rectangle_yuv420p_optimized(in_frame, detection.bbox, color,
                                     detection.thickness);
  }

  // If no legend, return the frame as is
  if (!legend_overlay_frame_ || !filter_graph_) {
    AVFramePtr result(av_frame_clone(in_frame));
    return result;
  }

  // Push frames through overlay filter
  ret = av_buffersrc_add_frame_flags(buffersrc_ctx_main_, in_frame,
                                     AV_BUFFERSRC_FLAG_KEEP_REF);
  if (ret < 0) {
    LOGE_FFMPEG("Cannot add frame to buffer source.", ret);
    return nullptr;
  }

  legend_overlay_frame_->pts = in_frame->pts;
  ret = av_buffersrc_add_frame_flags(buffersrc_ctx_overlay_,
                                     legend_overlay_frame_.get(),
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

    for (int x = 0; x < ow; ++x) {
      int dx = dst_x + x;
      if (static_cast<unsigned>(dx) >= static_cast<unsigned>(W)) continue;

      const uint8_t a = Ao[x];
      if (a == 0) continue;
      if (a == 255) {
        Yd[dx] = Yo[x];
        continue;
      }

      // Alpha blending: dst = (dst * (255-a) + src * a) / 255
      // Optimized: dst = dst + ((src - dst) * a) / 255
      const int diff = static_cast<int>(Yo[x]) - static_cast<int>(Yd[dx]);
      Yd[dx] = static_cast<uint8_t>(Yd[dx] + (diff * a) / 255);
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

      const uint8_t* __restrict Co =
          overlay->data[c] + y * overlay->linesize[c];
      uint8_t* __restrict Cd = dst->data[c] + dy * dst->linesize[c];

      // Sample alpha from top-left of each 2x2 block
      const uint8_t* __restrict Ao =
          overlay->data[3] + (y * 2) * overlay->linesize[3];

      for (int x = 0; x < ow2; ++x) {
        int dx = (dst_x >> 1) + x;
        if (static_cast<unsigned>(dx) >= static_cast<unsigned>(Wc)) continue;

        const uint8_t a = Ao[x * 2];
        if (a == 0) continue;
        if (a == 255) {
          Cd[dx] = Co[x];
          continue;
        }

        const int diff = static_cast<int>(Co[x]) - static_cast<int>(Cd[dx]);
        Cd[dx] = static_cast<uint8_t>(Cd[dx] + (diff * a) / 255);
      }
    }
  }
}
} // namespace avs_analyzer