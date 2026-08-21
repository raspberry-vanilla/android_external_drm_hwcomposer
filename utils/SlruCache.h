/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace android::drm_hwcomposer {

inline constexpr size_t kDefaultMaxProbationary = 16;
inline constexpr size_t kDefaultMaxProtected = 48;

/**
 * Segmented Least Recently Used (SLRU) Cache.
 *
 * Divides capacity into two segments:
 * 1. Probationary: Newly inserted items start here. If probationary capacity is
 *    reached, the oldest probationary item is evicted.
 * 2. Protected: When an item in the probationary segment is accessed again
 * (Find), it gets promoted to the protected segment. If the protected segment
 * overflows, its oldest item is demoted back to probationary.
 *
 * This protects hot steady-state entries from being thrashed by a burst of
 * one-off transient keys (such as smooth brightness or color animations).
 *
 * Thread-safe: All public methods are synchronized with an internal mutex.
 */
template <typename Key, typename Value,
          size_t MaxProbationary = kDefaultMaxProbationary,
          size_t MaxProtected = kDefaultMaxProtected,
          typename Compare = std::less<Key>>
class SlruCache {
  static_assert(MaxProbationary > 0, "MaxProbationary must be > 0");
  static_assert(MaxProtected > 0, "MaxProtected must be > 0");

 public:
  auto Find(const Key &key) -> Value {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) {
      return Value{};
    }

    auto &entry = it->second;
    if (entry.segment == Segment::kProbationary) {
      PromoteToProtected(entry);
    } else {
      MoveToMru(entry);
    }

    return entry.val;
  }

  void Insert(const Key &key, Value val) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      it->second.val = std::move(val);
      MoveToMru(it->second);
      return;
    }

    if (probationary_.size() >= MaxProbationary) {
      EvictProbationary();
    }

    probationary_.push_back(key);
    auto list_it = std::prev(probationary_.end());
    map_.emplace(key, Entry{std::move(val), Segment::kProbationary, list_it});
  }

  auto Size() const -> size_t {
    const std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
  }
  auto ProbationarySize() const -> size_t {
    const std::lock_guard<std::mutex> lock(mutex_);
    return probationary_.size();
  }
  auto ProtectedSize() const -> size_t {
    const std::lock_guard<std::mutex> lock(mutex_);
    return protected_.size();
  }

  void Clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    map_.clear();
    probationary_.clear();
    protected_.clear();
  }

  template <typename Formatter>
  auto Dump(const Formatter &format_entry,
            std::string_view cache_name = "SLRU Cache") const -> std::string {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::stringstream ss;
    ss << cache_name << ": " << map_.size() << "/"
       << (MaxProbationary + MaxProtected)
       << " (Probationary: " << probationary_.size() << "/" << MaxProbationary
       << ", Protected: " << protected_.size() << "/" << MaxProtected << ")\n";
    for (const auto &key : protected_) {
      auto it = map_.find(key);
      if (it != map_.end()) {
        ss << "  [Protected]    " << format_entry(key, it->second.val) << "\n";
      }
    }
    for (const auto &key : probationary_) {
      auto it = map_.find(key);
      if (it != map_.end()) {
        ss << "  [Probationary] " << format_entry(key, it->second.val) << "\n";
      }
    }
    return ss.str();
  }

 private:
  enum class Segment { kProbationary, kProtected };

  struct Entry {
    Value val;
    Segment segment;
    typename std::list<Key>::iterator list_it;
  };

  void MoveToMru(Entry &entry) {
    auto &list = (entry.segment == Segment::kProtected) ? protected_
                                                        : probationary_;
    list.splice(list.end(), list, entry.list_it);
  }

  void EvictProbationary() {
    if (!probationary_.empty()) {
      map_.erase(probationary_.front());
      probationary_.pop_front();
    }
  }

  void DemoteProtected() {
    if (protected_.empty()) {
      return;
    }
    const Key &demoted_key = protected_.front();
    auto demoted_it = map_.find(demoted_key);
    if (demoted_it != map_.end()) {
      demoted_it->second.segment = Segment::kProbationary;
    }
    probationary_.splice(probationary_.end(), protected_, protected_.begin());
  }

  void PromoteToProtected(Entry &entry) {
    if (protected_.size() >= MaxProtected) {
      DemoteProtected();
    }
    protected_.splice(protected_.end(), probationary_, entry.list_it);
    entry.segment = Segment::kProtected;
  }

  mutable std::mutex mutex_;
  std::list<Key> probationary_;
  std::list<Key> protected_;
  std::map<Key, Entry, Compare> map_;
};

}  // namespace android::drm_hwcomposer
