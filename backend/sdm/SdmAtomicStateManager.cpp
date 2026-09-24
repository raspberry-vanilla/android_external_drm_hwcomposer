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

#include "backend/sdm/SdmAtomicStateManager.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include <core/buffer_allocator.h>  // IWYU pragma: keep
#include <core/sdm_types.h>
#include <sdm_display_intf_drawcycle.h>
#include <sdm_display_intf_lifecycle.h>
#include <sdm_display_intf_settings.h>
#include <utils/fence.h>

#include "backend/sdm/SnapAllocHandle.h"
#include "backend/sdm/sdm_error.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/AtomicStateManager.h"
#include "drm/DrmMode.h"
#include "hwc/HwcDisplay.h"
#include "utils/ColorUtil.h"
#include "utils/fd.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

using sdm_error::ErrorToString;

namespace {
std::optional<sdm::SDMPowerMode> ToSdmPowerMode(
    HwcDisplay::PowerMode power_mode) {
  switch (power_mode) {
    case HwcDisplay::PowerMode::kOff:
      return sdm::SDMPowerMode::POWER_MODE_OFF;
    case HwcDisplay::PowerMode::kOn:
      return sdm::SDMPowerMode::POWER_MODE_ON;
    default:
      return std::nullopt;
  }
}

// The sdm_color_mode is the same value as the AIDL color mode. Check the range
// and convert it to the stronger enum type.
std::optional<ColorMode> ToColorMode(int32_t sdm_color_mode) {
  if (sdm_color_mode < static_cast<int32_t>(ColorMode::kNative) ||
      sdm_color_mode > static_cast<int32_t>(ColorMode::kDisplayBt2020)) {
    ALOGW("Got an out of range ColorMode: %d", sdm_color_mode);
    return std::nullopt;
  }
  return static_cast<ColorMode>(sdm_color_mode);
}
}  // namespace

SdmAtomicStateManager::SdmAtomicStateManager(

    uint64_t sdm_display_id, sdm::SDMDisplayLifeCycleIntf* lifecycle_intf,
    sdm::SDMDisplayDrawCycleIntf* draw_cycle_intf,
    sdm::SDMDisplaySettingsIntf* settings_intf)
    : sdm_display_id_(sdm_display_id),
      lifecycle_intf_(lifecycle_intf),
      draw_cycle_intf_(draw_cycle_intf),
      settings_intf_(settings_intf) {
  InitColorModes();
  InitDisplayConfigs();

  // Disable SDM idle timeout mechanism. drmhwc has an idle timeout already,
  // and the SDM one seems to result in jank.
  auto display_error = settings_intf_->SetIdleTimeout(0);
  ALOGE_IF(display_error != sdm::kErrorNone, "Error disabling timeout: %s",
           ErrorToString(display_error).c_str());
}

void SdmAtomicStateManager::InitColorModes() {
  auto sdm_color_modes = GetSdmColorModes();
  for (auto sdm_mode : sdm_color_modes) {
    auto color_mode = ToColorMode(sdm_mode);
    if (!color_mode) {
      continue;
    }
    auto colorspace = ColorUtil::ToHwcColorspace(color_mode.value());
    auto it = colorspace_map_.find(colorspace);
    if (it != colorspace_map_.end()) {
      ALOGW(
          "ColorModes %d and %d both map to Colorspace %d. Skipping ColorMode "
          "%d",
          static_cast<int32_t>(*color_mode), static_cast<int32_t>(it->second),
          static_cast<int32_t>(it->first), static_cast<int32_t>(*color_mode));
      continue;
    }
    color_modes_.push_back(color_mode.value());
    colorspace_map_[colorspace] = color_modes_.back();
  }
  if (color_modes_.empty()) {
    ALOGE(
        "InitColorModes resulted in no ColorModes being set. Falling back to "
        "kNative.");
    color_modes_.push_back(ColorMode::kNative);
    colorspace_map_[HwcColorspace::kDefault] = ColorMode::kNative;
  }
}

std::vector<int32_t> SdmAtomicStateManager::GetSdmColorModes() const {
  uint32_t out_num_modes = 0;
  auto display_error = settings_intf_->GetColorModes(sdm_display_id_,
                                                     &out_num_modes, nullptr);
  if (display_error != sdm::kErrorNone) {
    ALOGE("GetColorModes failed: %s", ErrorToString(display_error).c_str());
    return {static_cast<int32_t>(ColorMode::kNative)};
  }
  std::vector<int32_t> sdm_color_modes(out_num_modes);
  display_error = settings_intf_->GetColorModes(sdm_display_id_, &out_num_modes,
                                                sdm_color_modes.data());
  if (display_error != sdm::kErrorNone) {
    ALOGE("GetColorModes failed: %s", ErrorToString(display_error).c_str());
    return {static_cast<int32_t>(ColorMode::kNative)};
  }
  return sdm_color_modes;
}

void SdmAtomicStateManager::InitDisplayConfigs() {
  auto error = settings_intf_->GetAllDisplayAttributes(sdm_display_id_,
                                                       &sdm_configs_);
  if (error != sdm::kErrorNone || sdm_configs_.empty()) {
    ALOGE("Failed to get display attributes from SDM: %s",
          ErrorToString(error).c_str());
  }

  uint32_t active_config_id = 0;
  error = settings_intf_->GetActiveConfig(sdm_display_id_, &active_config_id);
  if (error == sdm::kErrorNone) {
    active_sdm_config_id_ = active_config_id;
  } else {
    ALOGE("Failed to get active config: %s", ErrorToString(error).c_str());
  }
}

std::optional<uint32_t> SdmAtomicStateManager::GetSdmConfigId(
    const DrmMode& mode) const {
  uint32_t drm_width = mode.GetRawMode().hdisplay;
  uint32_t drm_height = mode.GetRawMode().vdisplay;
  // In Qualcomm SDM (HWDeviceDRM::PopulateDisplayAttributes in
  // sdm/libs/dal/hw_device_drm.cpp), SDM populates
  // DisplayConfigVariableInfo::fps directly from the integer mode.vrefresh, and
  // derives vsync_period_ns as UINT32(1000000000L / fps).
  //
  // Because SDM stores the refresh rate as an integer, fractional/NTSC refresh
  // rate modes (e.g. 59.94/59.95 Hz -> 60 Hz) lose fractional precision on the
  // SDM side. This causes fixed microsecond vsync period tolerance checks to
  // fail for valid fractional modes (like 2560x1440@59.95Hz).
  //
  // Match on width, height, and integral refresh rate (mode.vrefresh or
  // round(GetVRefresh())), and if multiple configs match, pick the one with the
  // closest vsync period.
  uint32_t drm_fps = mode.GetRawMode().vrefresh != 0
                         ? mode.GetRawMode().vrefresh
                         : static_cast<uint32_t>(
                               std::lround(mode.GetVRefresh()));
  int64_t drm_vsync = mode.GetVSyncPeriodNs();

  std::optional<uint32_t> best_config_idx;
  int64_t min_vsync_diff = std::numeric_limits<int64_t>::max();

  for (const auto& [config_idx, sdm_config] : sdm_configs_) {
    if (drm_width == sdm_config.x_pixels && drm_height == sdm_config.y_pixels &&
        drm_fps == sdm_config.fps) {
      int64_t diff = std::abs(drm_vsync -
                              static_cast<int64_t>(sdm_config.vsync_period_ns));
      if (diff < min_vsync_diff) {
        min_vsync_diff = diff;
        best_config_idx = config_idx;
      }
    }
  }

  return best_config_idx;
}

std::vector<DrmMode> SdmAtomicStateManager::FilterModes(
    const std::vector<DrmMode>& modes) const {
  std::vector<DrmMode> supported_modes;
  for (const auto& drm_mode : modes) {
    auto config_id = GetSdmConfigId(drm_mode);
    if (config_id.has_value()) {
      supported_modes.push_back(drm_mode);
      ALOGI("DRM Mode %s is supported (mapped to SDM Config %u)",
            drm_mode.GetName().c_str(), *config_id);
    }
  }
  return supported_modes;
}

std::unique_ptr<AtomicRequest> SdmAtomicStateManager::GetAtomicModeReqForArgs(
    AtomicCommitArgs& /*args*/) {
  ALOGE("GetAtomicModeReqForArgs not implemented for SdmAtomicStateManager");
  return nullptr;
}

std::optional<AtomicCommitResult> SdmAtomicStateManager::ExecuteAtomicCommit(
    AtomicCommitArgs& args) {
  AtomicCommitResult result = {};
  if (args.display_mode) {
    auto sdm_config_idx = GetSdmConfigId(*args.display_mode);
    if (sdm_config_idx.has_value()) {
      ALOGI("Setting active SDM config %u for mode %s", *sdm_config_idx,
            args.display_mode->GetName().c_str());
      sdm::DisplayError display_error = sdm::kErrorNone;
      // TODO (b/544508262): Use SetActiveConfig for both seamless and
      // non-seamless changes.
      if (args.seamless && IsSeamlessConfigChange(*args.display_mode)) {
        ALOGV("Executing seamless mode change using SetActiveConfigIndex");
        display_error = draw_cycle_intf_->SetActiveConfigIndex(sdm_display_id_,
                                                               *sdm_config_idx);
        if (display_error != sdm::kErrorNone) {
          ALOGE("SetActiveConfigIndex failed: %s",
                ErrorToString(display_error).c_str());
          return std::nullopt;
        }
      } else {
        ALOGV("Executing non-seamless mode change using SetActiveConfig");
        display_error = settings_intf_->SetActiveConfig(sdm_display_id_,
                                                        *sdm_config_idx);
        if (display_error != sdm::kErrorNone) {
          ALOGE("SetActiveConfig failed: %s",
                ErrorToString(display_error).c_str());
          return std::nullopt;
        }
      }
      active_sdm_config_id_ = sdm_config_idx;
    } else {
      ALOGE("Failed to find SDM config for DRM mode %s (vsync %d ns)",
            args.display_mode->GetName().c_str(),
            args.display_mode->GetVSyncPeriodNs());
      return std::nullopt;
    }
  }
  if (args.power_mode) {
    auto new_power_mode = ToSdmPowerMode(*args.power_mode);
    ALOGW_IF(!new_power_mode.has_value(), "Unsupported power_mode: %d",
             static_cast<int>(*args.power_mode));
    if (new_power_mode) {
      auto display_error = lifecycle_intf_->SetPowerMode(sdm_display_id_,
                                                         static_cast<int32_t>(
                                                             *new_power_mode));
      if (display_error != sdm::kErrorNone) {
        ALOGE("SetPowerMode failed: %s", ErrorToString(display_error).c_str());
        return std::nullopt;
      }
      power_mode_ = *new_power_mode;
    }
  }
  if (args.colorspace && args.colorspace != last_colorspace_) {
    ColorMode color_mode = ColorMode::kNative;
    auto it = colorspace_map_.find(*args.colorspace);
    ALOGE_IF(it == colorspace_map_.end(),
             "Did not find ColorMode for HwcColorspace %d",
             static_cast<int>(*args.colorspace));
    if (it != colorspace_map_.end()) {
      color_mode = it->second;
    }
    // Hardcode to colorimetric render intent.
    constexpr int32_t kColorimetricRenderIntent = 0;
    auto display_error = settings_intf_->SetColorModeWithRenderIntent(
        sdm_display_id_, static_cast<int32_t>(color_mode),
        kColorimetricRenderIntent);
    if (display_error != sdm::kErrorNone) {
      ALOGE("SetColorModeWithRenderIntent failed: %s",
            ErrorToString(display_error).c_str());
      return std::nullopt;
    }
    last_colorspace_ = args.colorspace;
  }
  if (args.color_matrix &&
      (!last_color_matrix_ || *args.color_matrix != *last_color_matrix_)) {
    auto display_error = settings_intf_->SetColorTransform(
        sdm_display_id_, std::vector<float>(args.color_matrix->begin(),
                                            args.color_matrix->end()));
    if (display_error != sdm::kErrorNone) {
      ALOGE("SetColorTransform failed: %s",
            ErrorToString(display_error).c_str());
      return std::nullopt;
    }
    last_color_matrix_ = args.color_matrix;
  }
  if (args.composition) {
    if (!UpdateClientTarget(*args.composition)) {
      ALOGE("UpdateClientTarget failed");
      return std::nullopt;
    }

    std::shared_ptr<sdm::Fence> out_retire_fence;
    auto display_error = draw_cycle_intf_->PresentDisplay(sdm_display_id_,
                                                          &out_retire_fence);
    if (display_error != sdm::kErrorNone) {
      ALOGE("PresentDisplay failed: %s", ErrorToString(display_error).c_str());
      return std::nullopt;
    }
    if (out_retire_fence) {
      // Dup to take ownership of new fence.
      result.present_fence = MakeSharedFd(sdm::Fence::Dup(out_retire_fence));
    }
  }
  return result;
}

void SdmAtomicStateManager::WaitLastFrame() {
  // TODO: Implement this.
}

bool SdmAtomicStateManager::IsActive() const {
  return power_mode_ != sdm::SDMPowerMode::POWER_MODE_OFF;
}

std::vector<ColorMode> SdmAtomicStateManager::GetColorModes() const {
  return color_modes_;
}

bool SdmAtomicStateManager::UpdateClientTarget(
    const LayerToPlaneJoiningPlan& plan) {
  if (!plan.client_z_order.has_value()) {
    // No client target to update.
    return true;
  }
  if (*plan.client_z_order < 0 ||
      static_cast<size_t>(*plan.client_z_order) >= plan.plan.size()) {
    ALOGE("client_z_order is out of bounds: %d >= %d", *plan.client_z_order,
          (int)plan.plan.size());
    return false;
  }
  const LayerData* layer_data = &plan.plan[*plan.client_z_order].layer;
  if (!layer_data->bi.has_value()) {
    ALOGE("Client target layer has no BufferInfo");
    return false;
  }
  std::shared_ptr<SnapAllocHandle>
      snap_handle = std::static_pointer_cast<SnapAllocHandle>(
          layer_data->bi->fds_shared);
  std::shared_ptr<sdm::Fence> acquire_fence;
  if (layer_data->acquire_fence != nullptr) {
    acquire_fence = sdm::Fence::Create(DupFd(layer_data->acquire_fence),
                                       "client target acquire_fence");
  }

  // TODO: Plumb the client target damage regions.
  sdm::SDMRegion region{0, {}};
  // TODO: What should these be filled out to?
  int32_t dataspace = 0;
  uint32_t version = 0;
  auto display_error = draw_cycle_intf_
                           ->SetClientTarget(sdm_display_id_,
                                             snap_handle->GetSnapHandle(),
                                             acquire_fence, dataspace, region,
                                             version);
  ALOGE_IF(display_error != sdm::kErrorNone, "SetClientTarget failed: %s",
           ErrorToString(display_error).c_str());
  return display_error == sdm::kErrorNone;
}

bool SdmAtomicStateManager::IsSeamlessConfigChange(const DrmMode& mode) const {
  if (!active_sdm_config_id_.has_value()) {
    ALOGW("IsSeamlessConfigChange: no active config cached");
    return false;
  }
  auto target_config_idx = GetSdmConfigId(mode);
  if (!target_config_idx.has_value()) {
    ALOGW("IsSeamlessConfigChange: target mode not mapped to SDM config");
    return false;
  }

  if (*target_config_idx == *active_sdm_config_id_) {
    ALOGD("IsSeamlessConfigChange: same config %u", *target_config_idx);
    return true;
  }

  auto current_config_it = sdm_configs_.find(*active_sdm_config_id_);
  auto target_config_it = sdm_configs_.find(*target_config_idx);

  if (current_config_it == sdm_configs_.end() ||
      target_config_it == sdm_configs_.end()) {
    ALOGW("IsSeamlessConfigChange: config info not found for %u or %u",
          *active_sdm_config_id_, *target_config_idx);
    return false;
  }

  int current_group = settings_intf_
                          ->GetDisplayConfigGroup(sdm_display_id_,
                                                  current_config_it->second);
  int target_group = settings_intf_
                         ->GetDisplayConfigGroup(sdm_display_id_,
                                                 target_config_it->second);

  bool seamless = (current_group == target_group) && (current_group != -1);
  ALOGI(
      "IsSeamlessConfigChange: current %u (group %d), target %u (group %d), "
      "seamless: %d",
      *active_sdm_config_id_, current_group, *target_config_idx, target_group,
      seamless);
  return seamless;
}

}  // namespace android::drm_hwcomposer
