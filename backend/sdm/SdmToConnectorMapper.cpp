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

#define LOG_TAG "drmhwc"  // NOLINT(cppcoreguidelines-macro-usage)

#include "backend/sdm/SdmToConnectorMapper.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

#include <core/sdm_types.h>
#include <sdm_display_intf_caps.h>

#include "backend/sdm/sdm_error.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

using sdm_error::ErrorToString;

SdmToConnectorMapper::SdmToConnectorMapper(sdm::SDMDisplayCapsIntf* caps_intf)
    : caps_intf_(caps_intf) {
}

std::optional<uint32_t> SdmToConnectorMapper::Register(SdmDisplayId sdm_id) {
  if (caps_intf_ == nullptr) {
    ALOGE("SdmToConnectorMapper::Register: caps_intf_ is null");
    return std::nullopt;
  }

  int32_t disp_hw_id = -1;
  auto error = caps_intf_->GetDisplayHwId(sdm_id, &disp_hw_id);
  if (error != sdm::kErrorNone) {
    ALOGE("GetDisplayHwId failed for sdm display %" PRIu64 ": %s", sdm_id,
          ErrorToString(error).c_str());
    return std::nullopt;
  }

  if (disp_hw_id < 0) {
    ALOGE("GetDisplayHwId returned negative hw_id %d for sdm display %" PRIu64,
          disp_hw_id, sdm_id);
    return std::nullopt;
  }

  // In Qualcomm SDM, the display hardware ID encodes the DRM connector ID
  // in the lower 12 bits (mask 0xFFF).
  constexpr uint32_t kConnectorIdMask = 0xFFF;
  uint32_t connector_id = static_cast<uint32_t>(disp_hw_id) & kConnectorIdMask;
  if (connector_id == 0) {
    ALOGE("Invalid connector id 0 for sdm display %" PRIu64 " (hw_id=%d)",
          sdm_id, disp_hw_id);
    return std::nullopt;
  }

  std::scoped_lock lock(mutex_);
  auto* entry = FindBySdmIdLocked(sdm_id);
  if (entry != nullptr) {
    if (entry->connector_id == connector_id) {
      ALOGW("SdmToConnectorMapper::Register: sdm display %" PRIu64
            " already mapped to connector %u",
            sdm_id, connector_id);
      return connector_id;
    }
    ALOGE("SdmToConnectorMapper::Register: sdm display %" PRIu64
          " re-mapped to different connector %u (was %u), clearing callback",
          sdm_id, connector_id, entry->connector_id);
    entry->connector_id = connector_id;
    entry->refresh_callback = nullptr;
    return connector_id;
  }

  mappings_.push_back(DisplayMapping{
      .sdm_id = sdm_id,
      .connector_id = connector_id,
      .refresh_callback = nullptr,
  });
  ALOGI("SdmToConnectorMapper::Register: mapped sdm display %" PRIu64
        " to connector %u",
        sdm_id, connector_id);
  return connector_id;
}

void SdmToConnectorMapper::Unregister(SdmDisplayId sdm_id) {
  std::scoped_lock lock(mutex_);
  auto it = std::find_if(mappings_.begin(), mappings_.end(),
                         [sdm_id](const DisplayMapping& m) {
                           return m.sdm_id == sdm_id;
                         });
  if (it != mappings_.end()) {
    ALOGI("SdmToConnectorMapper::Unregister: unmapped sdm display %" PRIu64
          " (connector %u)",
          sdm_id, it->connector_id);
    mappings_.erase(it);
  } else {
    ALOGW("SdmToConnectorMapper::Unregister: sdm display %" PRIu64 " not found",
          sdm_id);
  }
}

void SdmToConnectorMapper::SetRefreshCallback(uint32_t connector_id,
                                              RefreshCallback callback) {
  std::scoped_lock lock(mutex_);
  auto* entry = FindByConnectorIdLocked(connector_id);
  if (entry == nullptr) {
    ALOGE(
        "SdmToConnectorMapper::SetRefreshCallback: No SDM mapping found for "
        "connector %u",
        connector_id);
    return;
  }
  entry->refresh_callback = std::move(callback);
}

void SdmToConnectorMapper::OnRefresh(SdmDisplayId sdm_id) {
  RefreshCallback callback = nullptr;
  {
    std::scoped_lock lock(mutex_);
    auto* entry = FindBySdmIdLocked(sdm_id);
    if (entry != nullptr) {
      callback = entry->refresh_callback;
    }
  }

  if (callback) {
    callback();
  } else {
    ALOGW(
        "SdmToConnectorMapper::OnRefresh: No refresh callback registered for "
        "sdm display %" PRIu64,
        sdm_id);
  }
}

std::optional<SdmToConnectorMapper::SdmDisplayId>
SdmToConnectorMapper::GetSdmIdForConnector(uint32_t connector_id) const {
  std::scoped_lock lock(mutex_);
  const auto* entry = FindByConnectorIdLocked(connector_id);
  if (entry != nullptr) {
    return entry->sdm_id;
  }
  return std::nullopt;
}

std::optional<uint32_t> SdmToConnectorMapper::GetConnectorIdForSdm(
    SdmDisplayId sdm_id) const {
  std::scoped_lock lock(mutex_);
  const auto* entry = FindBySdmIdLocked(sdm_id);
  if (entry != nullptr) {
    return entry->connector_id;
  }
  return std::nullopt;
}

SdmToConnectorMapper::DisplayMapping* SdmToConnectorMapper::FindBySdmIdLocked(
    SdmDisplayId sdm_id) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  return const_cast<DisplayMapping*>(
      std::as_const(*this).FindBySdmIdLocked(sdm_id));
}

const SdmToConnectorMapper::DisplayMapping*
SdmToConnectorMapper::FindBySdmIdLocked(SdmDisplayId sdm_id) const {
  auto it = std::find_if(mappings_.begin(), mappings_.end(),
                         [sdm_id](const DisplayMapping& m) {
                           return m.sdm_id == sdm_id;
                         });
  return it != mappings_.end() ? &(*it) : nullptr;
}

SdmToConnectorMapper::DisplayMapping*
SdmToConnectorMapper::FindByConnectorIdLocked(uint32_t connector_id) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  return const_cast<DisplayMapping*>(
      std::as_const(*this).FindByConnectorIdLocked(connector_id));
}

const SdmToConnectorMapper::DisplayMapping*
SdmToConnectorMapper::FindByConnectorIdLocked(uint32_t connector_id) const {
  auto it = std::find_if(mappings_.begin(), mappings_.end(),
                         [connector_id](const DisplayMapping& m) {
                           return m.connector_id == connector_id;
                         });
  return it != mappings_.end() ? &(*it) : nullptr;
}

}  // namespace android::drm_hwcomposer
