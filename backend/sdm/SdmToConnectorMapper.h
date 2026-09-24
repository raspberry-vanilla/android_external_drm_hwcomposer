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

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace sdm {
class SDMDisplayCapsIntf;
}  // namespace sdm

namespace android::drm_hwcomposer {

// SdmToConnectorMapper maps Qualcomm SDM display IDs to DRM connector IDs and
// manages the refresh callbacks associated with each connector.
class SdmToConnectorMapper {
 public:
  using SdmDisplayId = uint64_t;
  using RefreshCallback = std::function<void()>;

  explicit SdmToConnectorMapper(sdm::SDMDisplayCapsIntf* caps_intf);

  // Registers an SDM display ID, resolves its DRM connector ID via SDM display
  // capabilities, and tracks the mapping. Returns the resolved DRM connector ID
  // if successful.
  std::optional<uint32_t> Register(SdmDisplayId sdm_id);

  // Unregisters an SDM display ID upon disconnect.
  void Unregister(SdmDisplayId sdm_id);

  // Sets or clears (if callback is null) the refresh trigger callback for a DRM
  // connector.
  void SetRefreshCallback(uint32_t connector_id, RefreshCallback callback);

  // Invokes the registered refresh callback for the given SDM display ID.
  void OnRefresh(SdmDisplayId sdm_id);

  // Returns the SDM display ID mapped to the given DRM connector ID, if any.
  std::optional<SdmDisplayId> GetSdmIdForConnector(uint32_t connector_id) const;

  // Returns the DRM connector ID mapped to the given SDM display ID, if any.
  std::optional<uint32_t> GetConnectorIdForSdm(SdmDisplayId sdm_id) const;

 private:
  struct DisplayMapping {
    SdmDisplayId sdm_id{0};
    uint32_t connector_id{0};
    RefreshCallback refresh_callback{nullptr};
  };

  DisplayMapping* FindBySdmIdLocked(SdmDisplayId sdm_id);
  const DisplayMapping* FindBySdmIdLocked(SdmDisplayId sdm_id) const;
  DisplayMapping* FindByConnectorIdLocked(uint32_t connector_id);
  const DisplayMapping* FindByConnectorIdLocked(uint32_t connector_id) const;

  sdm::SDMDisplayCapsIntf* caps_intf_{nullptr};
  mutable std::mutex mutex_;
  std::vector<DisplayMapping> mappings_;
};

}  // namespace android::drm_hwcomposer
