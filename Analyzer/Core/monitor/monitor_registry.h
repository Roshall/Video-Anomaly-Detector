#ifndef ANALYZER_MONITOR_REGISTRY_H
#define ANALYZER_MONITOR_REGISTRY_H

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include <unordered_set>
#include <utility>

#include "analyzer.h"
#include "event.h"
#include "monitor.h"
#include "video_processing/dynamic_annotator.h"
#include "video_processing/video_context.h"

namespace avs_analyzer {

class AnalyzerRegistration;

// Everything the worker needs to detect one monitor's event. Contexts are
// reference counted so an in-flight detection snapshot stays alive after the
// monitor is removed; `active` tells the detect loop to stop using it.
// Analyzer/detector are shared_ptr so a snapshot can outlive the registry
// entry without requiring their complete types in this header.
struct MonitorContext {
  std::unique_ptr<Monitor> monitor;
  std::shared_ptr<Analyzer> analyzer;
  std::shared_ptr<EventDetector> detector;
  std::shared_ptr<AnalyzerRegistration> registration;
  std::vector<DynamicAnnotator::ROIEntry> roi_entries;
  EventDetectorConfig event_cfg;
  std::atomic<int> current_highest_level{0};
  std::atomic<bool> active{true};
};

// Snapshot-based monitor container. A caller copies the context list and
// releases the lock before doing any work, so inference and annotation never
// block monitor add/remove (and vice versa).
class MonitorRegistry {
 public:
  using Ptr = std::shared_ptr<MonitorContext>;

  void AddBatch(std::vector<Ptr> contexts);
  // Stable snapshot: removed contexts stay alive through the returned shared
  // pointers until the snapshot is destroyed.
  [[nodiscard]] std::vector<Ptr> Snapshot() const;
  // Marks matching contexts inactive and erases them. The caller must stop
  // their analyzers outside any registry lock.
  std::vector<Ptr> RemoveBatch(const std::vector<std::string>& codes);
  [[nodiscard]] std::vector<Monitor> GetAll() const;
  [[nodiscard]] bool Contains(const std::string& code) const;
  [[nodiscard]] bool Empty() const;
  [[nodiscard]] size_t Size() const;

  // One label per distinct task, used for the annotation legend.
  [[nodiscard]] std::vector<std::pair<Task, std::u8string>> TaskLabels() const;
  // True when any monitor requests the push output.
  [[nodiscard]] bool AnyPushStream() const;

  // Parses ROIs for the given video geometry on every context. Called on the
  // pull thread when a stream generation opens and from offline analysis.
  void PrepareForStream(const VideoContext& video_context) const;
  static void PrepareForStream(MonitorContext& ctx,
                               const VideoContext& video_context);

  // Wakes a detector blocked on the next inference result.
  static void WakeAnalyzers(const std::vector<Ptr>& contexts);
  // Stops analyzers and releases their detector registrations. Blocking work
  // must run outside the registry lock (callers pass a snapshot).
  static void StopAnalyzers(const std::vector<Ptr>& contexts);

 private:
  mutable std::shared_mutex mutex_;
  std::vector<Ptr> contexts_;
};

}  // namespace avs_analyzer

#endif  // ANALYZER_MONITOR_REGISTRY_H
