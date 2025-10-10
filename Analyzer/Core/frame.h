#ifndef ANALYZER_FRAME_H
#define ANALYZER_FRAME_H

#include "helper.h"

namespace avs_analyzer {
struct Frame {
  int64_t timestamp{}; // ms
  AVFramePtr data{};
  uint32_t object_count{};
  uint32_t HasObject() const { return object_count > 0; }
};

struct RawFrame {
  int64_t timestamp{};
  AVFrame* data{};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_FRAME_H