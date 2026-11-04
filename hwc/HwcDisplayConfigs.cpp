/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "HwcDisplayConfigs.h"

#include <drm/drm_mode.h>
#include <xf86drmMode.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <vector>

#include "backend/BackendDisplayCapabilities.h"
#include "drm/DrmConnector.h"
#include "drm/DrmMode.h"
#include "utils/log.h"

constexpr uint32_t kHeadlessModeDisplayWidthMm = 163;
constexpr uint32_t kHeadlessModeDisplayHeightMm = 122;
constexpr uint32_t kHeadlessModeDisplayWidthPx = 1024;
constexpr uint32_t kHeadlessModeDisplayHeightPx = 768;
constexpr uint32_t kHeadlessModeDisplayVRefresh = 60;
constexpr uint32_t kSyncLen = 10;
constexpr uint32_t kBackPorch = 10;
constexpr uint32_t kFrontPorch = 10;
constexpr uint32_t kHzInKHz = 1000;

namespace android::drm_hwcomposer {

HwcDisplayConfigs HwcDisplayConfigsGenerator::GetFakeMode(uint16_t width,
                                                          uint16_t height) {
  HwcDisplayConfigs configs;

  configs.preferred_config_id = next_config_id_++;
  auto headless_drm_mode_info = (drmModeModeInfo){
      .hdisplay = width,
      .vdisplay = height,
      .vrefresh = kHeadlessModeDisplayVRefresh,
      .name = "VIRTUAL-MODE",
  };

  if (width == 0 || height == 0) {
    strcpy(headless_drm_mode_info.name, "HEADLESS-MODE");
    headless_drm_mode_info.hdisplay = kHeadlessModeDisplayWidthPx;
    headless_drm_mode_info.vdisplay = kHeadlessModeDisplayHeightPx;
  }

  /* We need a valid mode to pass the kernel validation */

  headless_drm_mode_info.hsync_start = headless_drm_mode_info.hdisplay +
                                       kFrontPorch;
  headless_drm_mode_info.hsync_end = headless_drm_mode_info.hsync_start +
                                     kSyncLen;
  headless_drm_mode_info.htotal = headless_drm_mode_info.hsync_end + kBackPorch;

  headless_drm_mode_info.vsync_start = headless_drm_mode_info.vdisplay +
                                       kFrontPorch;
  headless_drm_mode_info.vsync_end = headless_drm_mode_info.vsync_start +
                                     kSyncLen;
  headless_drm_mode_info.vtotal = headless_drm_mode_info.vsync_end + kBackPorch;

  headless_drm_mode_info.clock = (headless_drm_mode_info.htotal *
                                  headless_drm_mode_info.vtotal *
                                  headless_drm_mode_info.vrefresh) /
                                 kHzInKHz;

  configs.hwc_configs[configs.preferred_config_id] = (HwcDisplayConfig){
      .id = configs.preferred_config_id,
      .group_id = 1,
      .mode = DrmMode(&headless_drm_mode_info),
      .output_type = OutputType::kSystem,
  };

  configs.mm_width = kHeadlessModeDisplayWidthMm;
  configs.mm_height = kHeadlessModeDisplayHeightMm;

  return configs;
}

std::optional<HwcDisplayConfigs>
HwcDisplayConfigsGenerator::GenerateDisplayConfigs(
    const DrmConnector &connector, const HwcConfigParameters &params) {
  if (connector.GetModes().empty()) {
    ALOGE("No modes reported by KMS");
    return std::nullopt;
  }

  HwcDisplayConfigs configs;
  configs.preferred_config_id = 0;
  configs.mm_width = connector.GetMmWidth();
  configs.mm_height = connector.GetMmHeight();

  // Devices configure HDR capability either through the generic DRM color
  // pipeline (e.g. Intel, MediaTek) or via backend capabilities overrides
  // (e.g. Qualcomm SDM).
  bool enable_hdr = params.use_color_pipeline;
  if (params.capabilities != nullptr) {
    auto override_types = params.capabilities->GetHdrTypesOverride();
    if (override_types.has_value()) {
      enable_hdr = !override_types.value().empty();
    }
  }
  enable_hdr = enable_hdr &&
               (connector.IsExternal() ? params.external_hdr_enabled
                                       : params.persistent_hdr_enabled);

  // Order determines preferred output type
  const std::vector<OutputType>
      hwc_supported_output_types = enable_hdr
                                       ? std::vector<
                                             OutputType>{OutputType::kSystem,
                                                         OutputType::kSdr}
                                       : std::vector<OutputType>{
                                             OutputType::kSdr};

  uint32_t next_group_id = 1;

  std::vector<DrmMode> modes;
  modes.reserve(connector.GetModes().size());
  for (const auto &mode : connector.GetModes()) {
    if ((mode.GetRawMode().flags & DRM_MODE_FLAG_3D_MASK) != 0) {
      ALOGI("Skipping display mode %s (Modes with 3D flag aren't supported)",
            mode.GetName().c_str());
      continue;
    }

    if (connector.IsInternal()) {
      if (params.min_refresh_rate.has_value() &&
          mode.GetVRefresh() < (params.min_refresh_rate.value() - 1)) {
        ALOGI("Skipping display mode %s (Refresh not above min)",
              mode.GetName().c_str());
        continue;
      }

      if (params.max_refresh_rate.has_value() &&
          mode.GetVRefresh() > (params.max_refresh_rate.value() + 1)) {
        ALOGI("Skipping display mode %s (Refresh not below max)",
              mode.GetName().c_str());
        continue;
      }

      if (params.force_disable_mrr &&
          (mode.GetRawMode().type & DRM_MODE_TYPE_PREFERRED) == 0) {
        ALOGI("Skipping display mode %s (MRR disabled)",
              mode.GetName().c_str());
        continue;
      }
    }

    modes.push_back(mode);
  }

  if (params.capabilities != nullptr) {
    modes = params.capabilities->FilterModes(modes);
  }

  for (const auto &output_type : hwc_supported_output_types) {
    for (const auto &mode : modes) {
      const ConfigId new_config_id = next_config_id_++;
      const uint32_t new_group_id = next_group_id++;
      configs.hwc_configs[new_config_id] = {
          .id = new_config_id,
          .group_id = new_group_id,
          .mode = mode,
          .output_type = output_type,
      };

      if ((mode.GetRawMode().type & DRM_MODE_TYPE_PREFERRED) != 0 &&
          configs.preferred_config_id == 0) {
        configs.preferred_config_id = new_config_id;
      }
    }
  }

  if (configs.hwc_configs.empty()) {
    ALOGE("No valid modes left after filtering");
    return std::nullopt;
  }

  /* We must have preferred mode. Set first mode as preferred
   * in case KMS haven't reported anything. */
  if (configs.preferred_config_id == 0) {
    ALOGW(
        "No preferred config reported by KMS. Falling back to the first "
        "config.");
    configs.preferred_config_id = configs.hwc_configs.begin()->first;
  }

  return configs;
}

bool HwcDisplayConfigs::SanitizeGroups() {
  /* A config group should not contain 2 modes with FPS delta less than ~1HZ
   * otherwise android.graphics.cts.SetFrameRateTest CTS will fail
   */
  constexpr float kMinFpsDelta = 1.0;
  std::unordered_set<ConfigId> configs_to_erase;

  for (const auto &[id1, config1] : hwc_configs) {
    if (configs_to_erase.count(id1) > 0) {
      continue;
    }

    for (const auto &[id2, config2] : hwc_configs) {
      if (id1 == id2) {
        continue;
      }

      if (config1.group_id != config2.group_id) {
        continue;
      }

      if (config1.output_type != config2.output_type) {
        continue;
      }

      if (configs_to_erase.count(id2) > 0) {
        continue;
      }

      if (fabsf(config1.mode.GetVRefresh() - config2.mode.GetVRefresh()) >=
          kMinFpsDelta) {
        continue;
      }

      ALOGI(
          "Group %i: Skipping display mode %s (Refresh rate value is "
          "too close to existing mode %s)",
          config2.group_id, config2.mode.GetName().c_str(),
          config1.mode.GetName().c_str());

      configs_to_erase.insert(id2);
    }
  }

  for (const auto &id : configs_to_erase) {
    hwc_configs.erase(id);
  }

  return true;
}

}  // namespace android::drm_hwcomposer
