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
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <core/display_interface.h>
#include <core/sdm_types.h>

#include "compositor/DisplayInfo.h"
#include "drm/AtomicStateManager.h"
#include "drm/DrmMode.h"

namespace sdm {
class SDMDisplayDrawCycleIntf;
class SDMDisplayLifeCycleIntf;
class SDMDisplaySettingsIntf;
}  // namespace sdm

namespace android::drm_hwcomposer {

struct LayerToPlaneJoiningPlan;

class SdmAtomicStateManager : public AtomicStateManager {
 public:
  SdmAtomicStateManager(uint64_t sdm_display_id,
                        sdm::SDMDisplayLifeCycleIntf* lifecycle_intf,
                        sdm::SDMDisplayDrawCycleIntf* draw_cycle_intf,
                        sdm::SDMDisplaySettingsIntf* settings_intf);
  ~SdmAtomicStateManager() override = default;

  // Execute the AtomicCommitArgs for this display through the SDM interfaces.
  std::optional<AtomicCommitResult> ExecuteAtomicCommit(AtomicCommitArgs& args);

  // AtomicStateManager:
  //
  // Not needed until multi-display commit is implemented.
  std::unique_ptr<AtomicRequest> GetAtomicModeReqForArgs(
      AtomicCommitArgs& args) override;

  void WaitLastFrame() override;
  bool IsActive() const override;

  // Returns the ColorModes that this SDM display supports.
  std::vector<ColorMode> GetColorModes() const;

  // Filters DRM modes to ensure they map to an SDM config id.
  std::vector<DrmMode> FilterModes(const std::vector<DrmMode>& modes) const;

  bool IsSeamlessConfigChange(const DrmMode& mode) const;

 private:
  // If the LayerToPlaneJoiningPlan has a client target, update the client
  // target in SDM.
  bool UpdateClientTarget(const LayerToPlaneJoiningPlan& plan);

  // Initialize color_modes_ and colorspace_map_.
  void InitColorModes();

  // Get the supported color modes from the SDMDisplaySettingsIntf.
  std::vector<int32_t> GetSdmColorModes() const;

  void InitDisplayConfigs();
  std::optional<uint32_t> GetSdmConfigId(const DrmMode& mode) const;

  uint64_t sdm_display_id_;
  sdm::SDMDisplayLifeCycleIntf* lifecycle_intf_;
  sdm::SDMDisplayDrawCycleIntf* draw_cycle_intf_;
  sdm::SDMDisplaySettingsIntf* settings_intf_;
  std::vector<ColorMode> color_modes_;
  std::map<HwcColorspace, ColorMode> colorspace_map_;
  sdm::SDMPowerMode power_mode_ = sdm::SDMPowerMode::POWER_MODE_OFF;
  // Colorspace/color matrix last successfully applied to SDM, so
  // ExecuteAtomicCommit can skip re-applying an unchanged value on every
  // commit.
  std::optional<HwcColorspace> last_colorspace_;
  std::shared_ptr<const HalColorTransformMatrix> last_color_matrix_;
  std::map<uint32_t, sdm::DisplayConfigVariableInfo> sdm_configs_;
  std::optional<uint32_t> active_sdm_config_id_;
};

}  // namespace android::drm_hwcomposer
