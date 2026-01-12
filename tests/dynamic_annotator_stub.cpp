#include "video_processing/dynamic_annotator.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace avs_analyzer {
AVFramePtr DynamicAnnotator::draw_elements(AVFrame* frame,
                                          const std::vector<DynamicAnnotator::ROIEntry>&,
                                          const std::vector<DynamicAnnotator::BBoxEntry>&) const {
  // Return a clone to simulate a successful annotation without pulling in
  // all dynamic annotator dependencies in unit test builds.
  AVFramePtr clone{av_frame_clone(frame)};
  return clone;
}
} // namespace avs_analyzer
