//
// Created by lg on 10/2/25.
//

#ifndef ANALYZER_ANNOTATOR_H
#define ANALYZER_ANNOTATOR_H

#include <opencv2/opencv.hpp>
#include <vector>

#include "helper.h"

struct AVFrame;
struct AVFilterGraph;
struct AVFilterContext;
namespace avs_analyzer {

struct Point;
// rectangles, color is always red
struct SimpleRect { int x, y, w, h, thickness; };
struct YUVColor { uint8_t y, u, v; };
class VideoOverlay {
 public:
  VideoOverlay() = default;
  VideoOverlay(const std::vector<cv::Point>& polygon_vertices, AVFrame* frame) {
    if (init(polygon_vertices, frame) < 0) {
      throw std::runtime_error("could not initialize overlay");
    }
  }

  // Initializes the filter graph with a fixed polygon.
  int init(const std::vector<cv::Point>& polygon_vertices, AVFrame* frame);

  // Draws dynamic rectangles on the input frame. The fixed polygon is also drawn.
  AVFramePtr draw_elements(AVFrame* in_frame,
                           const std::vector<cv::Rect>& rects, int thickness) const;

 private:
  YUVColor bbox_color_;
  AVFilterGraphPtr filter_graph;
  AVFilterContext* buffersrc_ctx_main = nullptr;
  AVFilterContext* buffersrc_ctx_overlay = nullptr;
  AVFilterContext* buffersink_ctx = nullptr;

  // We store the pre-rendered polygon frame here
  AVFramePtr polygon_overlay_frame;
  AVPixelFormat format{};

};
}


#endif  // ANALYZER_ANNOTATOR_H
