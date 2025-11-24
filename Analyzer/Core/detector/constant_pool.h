//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_CONSTANT_POOL_H
#define ANALYZER_CONSTANT_POOL_H
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
namespace avs_analyzer{

template<class Key, class T,
          class Hash = std::hash<Key>,
          class Eq   = std::equal_to<Key>>
class ConstPool {
 public:
  using value_type = T;
  using const_ptr  = std::shared_ptr<const T>;

  ConstPool() = default;
  ConstPool(const ConstPool&) = delete;
  ConstPool& operator=(const ConstPool&) = delete;

  // Get existing immutable instance by key, or create one atomically.
  // Perfect-forwards args to T's constructor.
  template<class... Args>
  const_ptr get_or_emplace(const Key& key, Args&&... args) {
    // Fast path: shared read
    {
      std::shared_lock lk(m_);
      if (auto it = map_.find(key); it != map_.end()) return it->second;
    }
    // Slow path: unique write + double-check
    std::unique_lock lk(m_);
    if (auto it = map_.find(key); it != map_.end()) return it->second;

    auto owned = std::make_shared<T>(std::forward<Args>(args)...);
    const_ptr immutable = owned;                 // upcast to const
    map_.emplace(key, immutable);
    return immutable;
  }

  // Return nullptr if not present.
  const_ptr get(const Key& key) const {
    std::shared_lock lk(m_);
    if (auto it = map_.find(key); it != map_.end()) return it->second;
    return {};
  }

  // Optional: erase by key. (Only removes from pool; outstanding shared_ptr keep it alive.)
  bool erase(const Key& key) {
    std::unique_lock lk(m_);
    return map_.erase(key) > 0;
  }

  std::size_t size() const {
    std::shared_lock lk(m_);
    return map_.size();
  }

  // Return all values in the pool
  std::vector<const_ptr> all() const {
    std::shared_lock lk(m_);
    std::vector<const_ptr> result;
    result.reserve(map_.size());
    for (const auto& [key, value] : map_) {
      result.push_back(value);
    }
    return result;
  }

  void clear() {
    std::unique_lock lk(m_);
    map_.clear();
  }

 private:
  mutable std::shared_mutex m_;
  std::unordered_map<Key, const_ptr, Hash, Eq> map_;
};
}
#endif  // ANALYZER_CONSTANT_POOL_H
