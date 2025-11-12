#ifndef ANALYZER_PAINTER_OPTIMIZED_H
#define ANALYZER_PAINTER_OPTIMIZED_H

#include "painter.h"
#include "painter_base.h"
#include "painter_shapes.h"
#include <vector>

namespace avs_analyzer {
// Factory function to create optimized frame
AVFrame* create_blank_yuva420p_optimized(int width, int height);

// Optimized batch operations
class BatchPainter {
public:
  explicit BatchPainter(AVFrame* frame);

  // Batch rectangle drawing
  void add_rectangle(const cv::Rect& rect, const YUVColor& color,
                     int thickness);
  void add_filled_rectangle(const cv::Rect& rect, const YUVColor& color);

  // Execute all batched operations
  void execute();
  void clear();

private:
  AVFrame* frame_;
  std::vector<std::tuple<cv::Rect, YUVColor, int>> rectangles_;
  std::vector<std::pair<cv::Rect, YUVColor>> filled_rectangles_;
};

// High-level convenience functions
void draw_detection_boxes(AVFrame* frame,
                          const std::vector<cv::Rect>& boxes,
                          const std::vector<YUVColor>& colors,
                          int thickness = 2);

void draw_tracking_paths(AVFrame* frame,
                         const std::vector<std::vector<cv::Point>>& paths,
                         const std::vector<YUVAColor>& colors,
                         int thickness = 2);
} // namespace avs_analyzer

#endif  // ANALYZER_PAINTER_OPTIMIZED_H