/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <cutils/native_handle.h>

#include <memory>
#include <optional>
#include <vector>

#include "bufferinfo/GrallocBufferHandle.h"
#include "compositor/LayerData.h"
#include "drm/AtomicStateManager.h"
#include "drm/CommitStatus.h"
#include "hwc/HwcDisplayConfigs.h"

namespace android::drm_hwcomposer {

class HwcDisplay {
 public:
  enum class ConfigError {
    kNone,
    kBadConfig,
  };

  const HwcDisplayConfig* GetCurrentConfig() const;
  std::vector<HwcDisplayConfig> GetDisplayConfigs() const;
  ConfigError SetConfig(ConfigId config);
  CommitStatusOr<AtomicCommitResult> ExecuteAtomicCommit(
      AtomicCommitArgs& a_args) const;
  AtomicCommitArgs CreateModesetCommit(
      const HwcDisplayConfig* config,
      const std::optional<LayerData>& modeset_layer);
};

// Stub for GrallocBufferHandle::Create
std::shared_ptr<GrallocBufferHandle> GrallocBufferHandle::Create(
    buffer_handle_t handle) {
  auto gralloc_handle = std::shared_ptr<GrallocBufferHandle>(
      new GrallocBufferHandle(handle));
  return gralloc_handle;
}

// Stub for GrallocBufferHandle::~GrallocBufferHandle
// Do not release the handle, since it wasn't imported.
GrallocBufferHandle::~GrallocBufferHandle() = default;

// NOLINTBEGIN(readability-convert-member-functions-to-static)
const HwcDisplayConfig* HwcDisplay::GetCurrentConfig() const {
  return nullptr;
}

std::vector<HwcDisplayConfig> HwcDisplay::GetDisplayConfigs() const {
  return {};
}

HwcDisplay::ConfigError HwcDisplay::SetConfig(ConfigId /*config*/) {
  return ConfigError::kNone;
}

CommitStatusOr<AtomicCommitResult> HwcDisplay::ExecuteAtomicCommit(
    AtomicCommitArgs& /*a_args*/) const {
  return CommitStatusOr<AtomicCommitResult>(CommitStatus{.error_code = 0});
}

AtomicCommitArgs HwcDisplay::CreateModesetCommit(
    const HwcDisplayConfig* /*config*/,
    const std::optional<LayerData>& /*modeset_layer*/) {
  return AtomicCommitArgs{};
}
// NOLINTEND(readability-convert-member-functions-to-static)

}  // namespace android::drm_hwcomposer
