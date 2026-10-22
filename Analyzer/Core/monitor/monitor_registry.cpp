#include "monitor_registry.h"

#include <algorithm>
#include <utility>

#include "Utils/log.h"

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

}  // namespace avs_analyzer
