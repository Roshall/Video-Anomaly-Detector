#ifndef ANALYZER_FRAME_H
#define ANALYZER_FRAME_H
#include <mutex>
#include <queue>
#include <vector>

#include "helper.h"

namespace avs_analyzer {
struct Frame {
  size_t timestamp{};
  uint32_t object_count{};
  std::shared_ptr<AVFrame> data{};
  uint32_t HasObject() const { return object_count > 0; }
};

}  // namespace avs_analyzer

#endif  // ANALYZER_FRAME_H