/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include <array>
#include <cstdint>
#include <map>
#include <memory>

#include "bufferinfo/BufferInfo.h"
#include "utils/fd.h"

#ifndef DRM_FORMAT_INVALID
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DRM_FORMAT_INVALID 0
#endif

using GemHandle = uint32_t;

namespace android::drm_hwcomposer {

class DrmDevice;

// Interface purely for testing/mocking.
class IDrmFbIdHandle {
 public:
  IDrmFbIdHandle() = default;
  virtual ~IDrmFbIdHandle() = default;
  IDrmFbIdHandle(IDrmFbIdHandle &&) = delete;
  IDrmFbIdHandle(const IDrmFbIdHandle &) = delete;
  auto operator=(const IDrmFbIdHandle &) = delete;
  auto operator=(IDrmFbIdHandle &&) = delete;

  virtual auto GetFbId [[nodiscard]] () const -> uint32_t = 0;
};

class DrmFbIdHandle : public IDrmFbIdHandle {
 public:
  static auto CreateInstance(BufferInfo *bo, GemHandle first_gem_handle,
                             DrmDevice &drm) -> std::shared_ptr<DrmFbIdHandle>;

  ~DrmFbIdHandle() override;
  DrmFbIdHandle(DrmFbIdHandle &&) = delete;
  DrmFbIdHandle(const DrmFbIdHandle &) = delete;
  auto operator=(const DrmFbIdHandle &) = delete;
  auto operator=(DrmFbIdHandle &&) = delete;

  auto GetFbId [[nodiscard]] () const -> uint32_t override {
    return fb_id_;
  }

 private:
  explicit DrmFbIdHandle(DrmDevice &drm);

  SharedFd drm_fd_;

  uint32_t fb_id_{};
  std::array<GemHandle, kBufferMaxPlanes> gem_handles_{};
};

// Interface for importing a drm framebuffer from a BufferInfo object.
class DrmFbImporter {
 public:
  virtual auto GetOrCreateFbId(BufferInfo *bo)
      -> std::shared_ptr<DrmFbIdHandle> = 0;
  virtual ~DrmFbImporter() = default;
};

// Implementation of DrmFbImporter which caches imported framebuffers and tracks
// their lifetime using std::weak_ptr.
class DrmFbCachedImporter : public DrmFbImporter {
 public:
  explicit DrmFbCachedImporter(DrmDevice &drm) : drm_(&drm) {};
  ~DrmFbCachedImporter() override = default;
  DrmFbCachedImporter(const DrmFbCachedImporter &) = delete;
  DrmFbCachedImporter(DrmFbCachedImporter &&) = delete;
  auto operator=(const DrmFbCachedImporter &) = delete;
  auto operator=(DrmFbCachedImporter &&) = delete;

  auto GetOrCreateFbId(BufferInfo *bo)
      -> std::shared_ptr<DrmFbIdHandle> override;

 private:
  void CleanupEmptyCacheElements() {
    for (auto it = drm_fb_id_handle_cache_.begin();
         it != drm_fb_id_handle_cache_.end();) {
      if (it->second.expired()) {
        it = drm_fb_id_handle_cache_.erase(it);
      } else {
        ++it;
      }
    }
  }

  DrmDevice *const drm_;
  SharedFd drm_fd_;

  std::map<GemHandle, std::weak_ptr<DrmFbIdHandle>> drm_fb_id_handle_cache_;
};

}  // namespace android::drm_hwcomposer
