#include "admission.h"

#include <algorithm>

namespace avs_analyzer {

AdmissionResult AdmitFrame(const std::vector<AdmissionEntry>& entries,
                           uint64_t frame_ts) {
  AdmissionResult result;
  for (const auto& entry : entries) {
    if (entry.analyzer_id == 0) {
      continue;
    }
    if (frame_ts < entry.wanted_frame_ts) {
      ++result.skipped;
      continue;
    }
    const auto it = std::find_if(
        result.groups.begin(), result.groups.end(),
        [&entry](const AdmissionGroup& group) { return group.task == entry.task; });
    if (it == result.groups.end()) {
      AdmissionGroup group;
      group.task = entry.task;
      group.analyzer_ids.push_back(entry.analyzer_id);
      result.groups.push_back(std::move(group));
    } else {
      it->analyzer_ids.push_back(entry.analyzer_id);
    }
  }
  for (auto& group : result.groups) {
    if (group.analyzer_ids.size() > 1) {
      group.shared = std::make_shared<SharedInference>();
    }
  }
  return result;
}

}  // namespace avs_analyzer
