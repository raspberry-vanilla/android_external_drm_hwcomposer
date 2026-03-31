/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <cstdint>
#include <map>
#include <optional>
#include <tuple>

#include "drm/AtomicCommitSink.h"
#include "drm/DrmUnique.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

struct AtomicCommitArgs;
struct BufferInfo;
class DrmConnector;
class DrmCrtc;
class DrmEncoder;
class DrmFbImporter;
class DrmPlane;
class DrmProperty;
class ResourceManager;
class HwcDisplay;

class DrmDevice {
  friend class FakeDrmDevice;
  friend class FakeCompositorDrmDevice;

 public:
  ~DrmDevice();

  static auto CreateInstance(std::string const &path, ResourceManager *res_man,
                             uint32_t index) -> std::unique_ptr<DrmDevice>;

  auto &GetFd() const {
    return fd_;
  }

  auto GetIndexInDevArray() const {
    return index_in_dev_array_;
  }

  auto &GetResMan() {
    return *res_man_;
  }

  auto &GetAtomicCommitSink() {
    return *atomic_commit_sink_;
  }

  auto GetConnectors() -> const std::vector<std::unique_ptr<DrmConnector>> &;
  auto GetPlanes() -> const std::vector<std::unique_ptr<DrmPlane>> &;
  auto GetCrtcs() -> const std::vector<std::unique_ptr<DrmCrtc>> &;
  auto GetEncoders() -> const std::vector<std::unique_ptr<DrmEncoder>> &;

  auto GetWritebackConnectors()
      -> const std::vector<std::unique_ptr<DrmConnector>> & {
    return writeback_connectors_;
  }

  auto GetMinResolution() const {
    return min_resolution_;
  }

  auto GetMaxResolution() const {
    return max_resolution_;
  }

  std::string GetName() const;

  auto RegisterUserPropertyBlob(const void *data, size_t length) const
      -> DrmModeUserPropertyBlobUnique;

  auto HasAddFb2ModifiersSupport() const {
    return HasAddFb2ModifiersSupport_;
  }

  auto CreateBufferForModeset(uint32_t width, uint32_t height)
      -> std::optional<BufferInfo>;

  auto &GetDefaultFbImporter() {
    return *drm_fb_importer_;
  }

  DrmCrtc *FindCrtcById(uint32_t id) const;

  DrmEncoder *FindEncoderById(uint32_t id) const;

  int GetProperty(uint32_t obj_id, uint32_t obj_type, const char *prop_name,
                  DrmProperty *property) const;

  const std::optional<std::pair<uint64_t, uint64_t>> &GetCapCursorSize() const {
    return cap_cursor_size_;
  }

  auto RefreshConnectors() -> std::vector<std::unique_ptr<DrmConnector>>;
  auto ResetConnectorsAndCrtcs() -> void;

 private:
  explicit DrmDevice(ResourceManager *res_man, uint32_t index);
  auto Init(const char *path) -> int;

  static auto IsKMSDev(const char *path) -> bool;

  SharedFd fd_;
  const uint32_t index_in_dev_array_;

  std::vector<std::unique_ptr<DrmConnector>> connectors_;
  std::vector<std::unique_ptr<DrmConnector>> writeback_connectors_;
  std::vector<std::unique_ptr<DrmEncoder>> encoders_;
  std::vector<std::unique_ptr<DrmCrtc>> crtcs_;
  std::vector<std::unique_ptr<DrmPlane>> planes_;

  std::pair<uint32_t, uint32_t> min_resolution_;
  std::pair<uint32_t, uint32_t> max_resolution_;
  std::optional<std::pair<uint64_t, uint64_t>> cap_cursor_size_;

  bool HasAddFb2ModifiersSupport_{};

  std::unique_ptr<DrmFbImporter> drm_fb_importer_;
  std::unique_ptr<AtomicCommitSink> atomic_commit_sink_;

  ResourceManager *const res_man_;
};

}  // namespace android::drm_hwcomposer
