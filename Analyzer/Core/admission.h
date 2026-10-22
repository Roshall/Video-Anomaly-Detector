#ifndef ANALYZER_ADMISSION_H
#define ANALYZER_ADMISSION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "detector/mailbox.h"
#include "task.h"

namespace avs_analyzer {

// One active monitor considered for a decoded frame.
struct AdmissionEntry {
  unsigned long analyzer_id{0};
  Task task{};
  // Timestamp the analyzer's event detector is waiting for.
  uint64_t wanted_frame_ts{0};
};

// Monitors of the same task that were admitted for the same frame. They share
// one image and one inference result: the first session to pick the task runs
// the model and the rest reuse the published detections.
struct AdmissionGroup {
  Task task{};
  std::shared_ptr<SharedInference> shared;
  std::vector<unsigned long> analyzer_ids;
};

struct AdmissionResult {
  std::vector<AdmissionGroup> groups;
  size_t skipped{0};

  [[nodiscard]] size_t sinks() const {
    size_t count = 0;
    for (const auto& group : groups) {
      count += group.analyzer_ids.size();
    }
    return count;
  }
};

// Drops entries whose detector does not want this frame yet
// (`frame_ts < wanted_frame_ts`) and groups the rest by task. Groups with a
// single sink get no SharedInference because there is nothing to deduplicate.
AdmissionResult AdmitFrame(const std::vector<AdmissionEntry>& entries,
                           uint64_t frame_ts);

}  // namespace avs_analyzer

#endif  // ANALYZER_ADMISSION_H
