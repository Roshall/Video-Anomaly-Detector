#include "dynamic_annotator.h"
#include "painter.h"
#include "painter_shapes.h"
#include "painter.h"
#include <algorithm>
#include <stdexcept>
#include <format>

#include "painter_text.h"
#include "task.h"
#include "Utils/enum_utils.h"
#include <Utils/log.h>
#include <cstring>

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
        return {155, 48, 170};
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
  // Preserve existing context for check if the graph is already initialized
  const VideoContext prev_ctx = video_ctx_;
  video_ctx_ = video_ctx;
  legend_config_ = legend_config;

  // If the graph is already initialized for a different video context, do not
  // silently reconfigure it; the policy enforces single allocation and
  // non-recreation of the overlay and graph. If the video context changes,
  // return an error (unless filter_graph_initialized_ is false).
  if (filter_graph_initialized_) {
    // Basic mismatch check: width/height/pixfmt
    if (prev_ctx.width != video_ctx.width || prev_ctx.height != video_ctx.height || prev_ctx.format != video_ctx.format) {
      LOGE("DynamicAnnotator::init: filter graph already initialized with a different video context; refusing to reinit");
      return -1;
    }
  }

  // Ensure the legend overlay and filter graph are initialized once using the
  // entire Task enumeration. This will pre-allocate a YUVA frame and build a
  // single overlay filter graph that will be reused and never recreated.
  ensure_legend_initialized();

  return 0;
}

bool DynamicAnnotator::ensure_legend_initialized() {
  // If already initialized, just return success
  if (filter_graph_initialized_ && legend_overlay_frame_) return true;

  // Build labels and colors using the full Task enum set
  std::vector<Task> all_tasks;
  for (const auto t : get_enum_range<Task>()) {
    all_tasks.push_back(t);
  }
  if (all_tasks.empty()) return false;

  std::vector<std::u8string> labels_u8;
  labels_u8.reserve(all_tasks.size());
  std::vector<YUVAColor> colors;
  colors.reserve(all_tasks.size());
  for (const auto& t : all_tasks) {
    labels_u8.emplace_back(toChinese(t));
    colors.push_back(Task2ColorAlpha(t));
  }

  // Determine required legend size using accurate measurement
  const auto [legend_width, legend_height] = estimate_legend_size_accurate(
      legend_config_.font_path,
      legend_config_.font_size,
      legend_config_.handle_size,
      legend_config_.row_spacing,
      labels_u8,
      legend_config_.legend_style.text_padding,
      legend_config_.legend_style.frame_padding);

  // Calculate legend position
  legend_layout_ = estimate_legend_layout(
      legend_config_.font_size,
      legend_config_.handle_size,
      legend_config_.row_spacing,
      labels_u8,
      legend_config_.legend_style.text_padding,
      legend_config_.legend_style.frame_padding,
      video_ctx_.width,
      video_ctx_.height,
      legend_config_.position,
      legend_config_.margin_x,
      legend_config_.margin_y);

  // Pre-allocate overlay frame once using this legend size
  try {
    legend_overlay_frame_.reset(create_blank_yuva420p(legend_width, legend_height));
    cached_overlay_w_ = legend_width;
    cached_overlay_h_ = legend_height;

    // Initialize filter graph (only once)
    if (const int r = init_filter_graph(); r < 0) {
      legend_overlay_frame_.reset();
      LOGE("Failed initializing filter graph for legend overlay: {}", r);
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    legend_overlay_frame_.reset();
    LOGE("Failed to allocate legend overlay frame in ensure_legend_initialized: {}", e.what());
    return false;
  }
}

void DynamicAnnotator::update_legend(const std::vector<Task>& tasks) {
  // If ensure_legend_initialized failed, we cannot draw
  if (!ensure_legend_initialized()) {
    LOGW("Legend overlay not initialized; cannot update legend");
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
      legend_config_.legend_style.text_padding,
      legend_config_.legend_style.frame_padding
      );

  // Do not modify legend_layout_ here: we allocated and configured the filter
  // graph using the layout determined when the overlay was pre-allocated in
  // ensure_legend_initialized(). The overlay position cannot be changed
  // without reconfiguring the filter graph, and that violates the single
  // allocation requirement. Only redraw the legend content in-place.

  // We pre-allocated the legend overlay using the full Task enum at init.
  // Here, we will only redraw into the existing overlay buffer; do not
  // reallocate or reinitialize filter graph under any circumstance.
  try {
    if (!legend_overlay_frame_) {
      LOGW("Legend overlay frame missing; cannot update legend");
      return;
    }
    // If the requested legend size is larger than the pre-allocated overlay,
    // log a warning but do not reallocate (we will clip the content)
    if (legend_width > cached_overlay_w_ || legend_height > cached_overlay_h_) {
      LOGW("Requested legend size ({},{}) exceeds pre-allocated overlay ({},{}). Clipping will occur.", legend_width, legend_height, cached_overlay_w_, cached_overlay_h_);
    }
    // Clear the buffer before re-drawing
    clear_legend_overlay();
    // Draw legend in the buffer
    draw_legend_yuva420p(
        legend_overlay_frame_.get(),
        legend_config_.font_path,
        {legend_config_.legend_style.frame_padding, legend_config_.legend_style.frame_padding + legend_config_.font_size},
        legend_config_.font_size,
        legend_config_.handle_size,
        legend_config_.row_spacing,
        colors,
        labels_u8,
        legend_config_.legend_style,
        true
    );
      LOGD("Redrew legend in-place into pre-allocated overlay ({}x{})", cached_overlay_w_, cached_overlay_h_);
  } catch (const std::exception& e) {
    LOGE("Failed to update legend overlay frame: {}", e.what());
  }
}

int DynamicAnnotator::init_filter_graph() {
  if (filter_graph_initialized_) {
    return 0;
  }

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

    // Overlay filter expects a YUVA420P overlay frame of the legend size (small overlay)
    const int overlay_w = legend_overlay_frame_->width;
    const int overlay_h = legend_overlay_frame_->height;
    // We only get here if the graph was not yet initialized. Proceed to create
    // the graph for the pre-allocated overlay frame.
    const auto args_overlay = std::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect=1/1",
      overlay_w, overlay_h,
      av_get_pix_fmt_name(AV_PIX_FMT_YUVA420P),
      tnum, tden);

  // Overlay filter positioned at legend_layout_ coordinates
  const std::string filter_descr = std::format(
      "[in][legend]overlay=x={}:y={}:format=auto[out]",
      legend_layout_.pos_x, legend_layout_.pos_y);

  // If a graph exists, and we need to reinit, clear it first
  if (filter_graph_) {
    filter_graph_.reset();
    buffersrc_ctx_main_ = nullptr;
    buffersrc_ctx_overlay_ = nullptr;
    buffersink_ctx_ = nullptr;
    filter_graph_initialized_ = false;
  }
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
  // Record cached overlay size and mark graph as initialized
  cached_overlay_w_ = overlay_w;
  cached_overlay_h_ = overlay_h;
  filter_graph_initialized_ = true;

  return 0;
}

void DynamicAnnotator::clear_legend_overlay() {
  if (!legend_overlay_frame_) return;
  // Ensure writable
  if (av_frame_make_writable(legend_overlay_frame_.get()) < 0) return;
  AVFrame* f = legend_overlay_frame_.get();
  const int w = f->width;
  const int h = f->height;
  // Y plane -> neutral 128
  for (int y = 0; y < h; ++y) {
    std::memset(f->data[0] + y * f->linesize[0], 128, static_cast<size_t>(w));
  }
  const int cw = (w + 1) >> 1;
  const int ch = (h + 1) >> 1;
  // U/V planes -> neutral 128
  for (int y = 0; y < ch; ++y) {
    std::memset(f->data[1] + y * f->linesize[1], 128, static_cast<size_t>(cw));
    std::memset(f->data[2] + y * f->linesize[2], 128, static_cast<size_t>(cw));
  }
  // A plane -> transparent
  for (int y = 0; y < h; ++y) {
    std::memset(f->data[3] + y * f->linesize[3], 0, static_cast<size_t>(w));
  }
}

AVFramePtr DynamicAnnotator::draw_elements(AVFrame* in_frame,
                                           const std::vector<DynamicAnnotator::ROIEntry>& rois,
                                           const std::vector<DynamicAnnotator::BBoxEntry>&
                                           bboxes) const {
  if (!in_frame) return nullptr;

  // Make sure the frame is writable
  int ret = av_frame_make_writable(in_frame);
  if (ret < 0) {
    LOGE_FFMPEG("Failed to make frame writable.", ret);
    return nullptr;
  }

  // Use a single BatchPainter to draw both bounding boxes and ROI outlines directly on the
  // YUV420P in_frame (no per-frame overlay allocation).
  BatchPainter painter(in_frame);

  // Draw bboxes: 'bboxes' is a vector of pairs<Task, cv::Rect>
  for (const auto& entry : bboxes) {
    const auto& task = entry.first;
    const auto& rect = entry.second;
    YUVColor color = Task2Color(task);
    if (static_cast<AVPixelFormat>(video_ctx_.format) == AV_PIX_FMT_YUV420P) {
      color = scale_for_limited(color);
    }
    painter.add_rectangle(rect, color, 2);
  }

  // Draw ROIs: vector of pairs<Task, polygon>
  for (const auto& entry : rois) {
    const auto& task = entry.first;
    const auto& polygon = entry.second;
    YUVColor pcolor = Task2Color(task);
    if (static_cast<AVPixelFormat>(video_ctx_.format) == AV_PIX_FMT_YUV420P) {
      pcolor = scale_for_limited(pcolor);
    }
    // Default ROI thickness: 4
    painter.add_polygon(polygon, pcolor, 4);
  }

  // Execute all batched operations to draw directly into in_frame
  painter.execute();

  // Legend is pre-rendered; it will be pushed directly into the overlay buffer source

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

  // Push legend overlay frame (small YUVA overlay) directly to buffersrc_ctx_overlay_
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