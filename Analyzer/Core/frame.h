#ifndef ANALYZER_FRAME_H
#define ANALYZER_FRAME_H

#include "helper.h"

namespace avs_analyzer {
struct Frame {
  uint32_t object_count{};
  std::shared_ptr<AVFrame> data{};
  uint32_t HasObject() const { return object_count > 0; }
};

}  // namespace avs_analyzer

#endif  // ANALYZER_FRAME_H