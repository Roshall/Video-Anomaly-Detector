#include "monitor_registry.h"

#include <algorithm>
#include <utility>

#include "Utils/log.h"
#include "task.h"

namespace avs_analyzer {

void MonitorRegistry::AddBatch(std::vector<Ptr> contexts) {
  if (contexts.empty()) {
    return;
  }
  std::unique_lock lock(mutex_);
  contexts_.reserve(contexts_.size() + contexts.size());
  for (auto& ctx : contexts) {
    if (ctx) {
      contexts_.push_back(std::move(ctx));
    }
  }
}

std::vector<MonitorRegistry::Ptr> MonitorRegistry::Snapshot() const {
  std::shared_lock lock(mutex_);
  return contexts_;
}

std::vector<MonitorRegistry::Ptr> MonitorRegistry::RemoveBatch(
    const std::vector<std::string>& codes) {
  std::vector<Ptr> removed;
  if (codes.empty()) {
    return removed;
  }
  std::unique_lock lock(mutex_);
  for (const auto& code : codes) {
    const auto it = std::find_if(
        contexts_.begin(), contexts_.end(), [&code](const Ptr& ctx) {
          return ctx && ctx->monitor && ctx->monitor->code == code;
        });
    if (it == contexts_.end()) {
      LOGW("trying to remove unknown monitor code: {}", code);
      continue;
    }
    (*it)->active.store(false, std::memory_order_release);
    removed.push_back(std::move(*it));
    contexts_.erase(it);
  }
  return removed;
}

std::vector<Monitor> MonitorRegistry::GetAll() const {
  const auto snapshot = Snapshot();
  std::vector<Monitor> monitors;
  monitors.reserve(snapshot.size());
  for (const auto& ctx : snapshot) {
    if (ctx && ctx->monitor) {
      monitors.push_back(*ctx->monitor);
    }
  }
  return monitors;
}

bool MonitorRegistry::Contains(const std::string& code) const {
  std::shared_lock lock(mutex_);
  return std::any_of(contexts_.begin(), contexts_.end(), [&code](const Ptr& ctx) {
    return ctx && ctx->monitor && ctx->monitor->code == code;
  });
}

bool MonitorRegistry::Empty() const {
  std::shared_lock lock(mutex_);
  return contexts_.empty();
}

size_t MonitorRegistry::Size() const {
  std::shared_lock lock(mutex_);
  return contexts_.size();
}

std::vector<std::pair<Task, std::u8string>> MonitorRegistry::TaskLabels() const {
  const auto snapshot = Snapshot();
  std::vector<std::pair<Task, std::u8string>> tasks;
  std::unordered_set<Task> seen;
  tasks.reserve(snapshot.size());
  for (const auto& ctx : snapshot) {
    if (!ctx || !ctx->monitor) {
      continue;
    }
    const auto task = ctx->monitor->task;
    if (!seen.insert(task).second) {
      continue;  // one label per distinct task
    }
    const auto label = ctx->monitor->behavior.empty()
                           ? toChinese(task)
                           : ctx->monitor->behavior;
    tasks.emplace_back(task, label);
  }
  return tasks;
}

bool MonitorRegistry::AnyPushStream() const {
  const auto snapshot = Snapshot();
  return std::any_of(snapshot.begin(), snapshot.end(), [](const auto& ctx) {
    return ctx && ctx->monitor && ctx->monitor->push_stream;
  });
}

void MonitorRegistry::PrepareForStream(const VideoContext& video_context) const {
  for (const auto& ctx : Snapshot()) {
    if (ctx) {
      PrepareForStream(*ctx, video_context);
    }
  }
}

void MonitorRegistry::PrepareForStream(MonitorContext& ctx,
                                       const VideoContext& video_context) {
  if (!ctx.monitor || !ctx.analyzer) {
    return;
  }
  if (video_context.width <= 0 || video_context.height <= 0) {
    return;
  }
  auto& monitor = *ctx.monitor;
  if (!monitor.ParseRecognitionRegion(video_context.width,
                                      video_context.height)) {
    LOGE("ParseRecognitionRegion() error for monitor {}", monitor.code);
    return;
  }
  if (monitor.task == Task::kBunkOff) {
    ctx.analyzer->SetROIs(monitor.roi_polygons);
  } else {
    ctx.analyzer->SetROIs({monitor.roi_points});
  }
  ctx.roi_entries.clear();
  if (!monitor.roi_points.empty()) {
    ctx.roi_entries.emplace_back(monitor.task, monitor.roi_points);
  }
}

void MonitorRegistry::WakeAnalyzers(const std::vector<Ptr>& contexts) {
  for (const auto& ctx : contexts) {
    if (ctx && ctx->analyzer) {
      ctx->analyzer->Stop();
    }
  }
}

void MonitorRegistry::StopAnalyzers(const std::vector<Ptr>& contexts) {
  for (const auto& ctx : contexts) {
    if (!ctx || !ctx->analyzer) {
      continue;
    }
    ctx->analyzer->Stop();
    // Resetting the registration releases the mailbox; the analyzer also
    // holds a reference, so clear both.
    ctx->analyzer->SetRegistration(nullptr);
    ctx->registration.reset();
  }
}

}  // namespace avs_analyzer
