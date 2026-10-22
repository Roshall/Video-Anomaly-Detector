#ifndef ANALYZER_FRAME_H
#define ANALYZER_FRAME_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "helper.h"

namespace avs_analyzer {
struct Frame {
  int64_t timestamp{}; // ms
  AVFramePtr data{};
  uint32_t object_count{};
  uint32_t HasObject() const { return object_count > 0; }

  // Idle pre-roll frames are stored JPEG-encoded to keep memory bounded.
  // `data` is only populated for frames that are needed unencoded (active
  // events, annotated frames, or the live push pipeline).
  std::vector<uint8_t> jpeg{};
  bool encoded{false};
  // Approximate retained bytes, used by the event buffer's byte budget.
  size_t byte_size{};

  // Original timestamps so an encoded entry can be materialised back into an
  // AVFrame with the same pts/best_effort_timestamp/time_base.
  int64_t pts{};
  int64_t best_effort_pts{};
  AVRational time_base{0, 1};
};

struct RawFrame {
  int64_t timestamp{};
  AVFrame* data{};
};

}  // namespace avs_analyzer

#endif  // ANALYZER_FRAME_H
