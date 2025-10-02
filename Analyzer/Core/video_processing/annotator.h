//
// Created by lg on 10/2/25.
//

#ifndef ANALYZER_ANNOTATOR_H
#define ANALYZER_ANNOTATOR_H
extern "C" {

}

#include <opencv2/opencv.hpp>
#include <sstream>
#include <vector>
#include <string>
#include <stdexcept>

#include "helper.h"

struct AVFrame;
struct AVFilterGraph;
struct AVFilterContext;
namespace avs_analyzer {

struct Point;
// Simplified struct for rectangles, color is always red
struct SimpleRect { int x, y, w, h, thickness; };

class VideoOverlay {
 public:
  ~VideoOverlay();

  // Initializes the filter graph with a fixed polygon.
  int init(const std::vector<cv::Point>& polygon_vertices, AVFrame* frame);

  // Draws dynamic rectangles on the input frame. The fixed polygon is also drawn.
  AVFramePtr draw_elements(AVFrame* in_frame,
                           const std::vector<cv::Rect>& rects, int thickness);

 private:
  void cleanup();

  AVFilterGraph* filter_graph = nullptr;
  AVFilterContext* buffersrc_ctx_main = nullptr;
  AVFilterContext* buffersrc_ctx_overlay = nullptr;
  AVFilterContext* buffersink_ctx = nullptr;

  // We store the pre-rendered polygon frame here
  AVFrame* polygon_overlay_frame = nullptr;
  std::string last_bbox_str{};
  std::string args_main{};
  std::string args_overlay{};
  AVPixelFormat format;

};
}


#endif  // ANALYZER_ANNOTATOR_H
