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

#define LOG_TAG "drmhwc"

#include "HwcDisplayConfigs.h"

#include <cmath>
#include <cstring>

#include "compositor/DisplayInfo.h"
#include "drm/DrmConnector.h"

#include <cutils/properties.h>

constexpr uint32_t kHeadlessModeDisplayVRefresh = 60;
constexpr uint32_t kSyncLen = 10;
constexpr uint32_t kBackPorch = 10;
constexpr uint32_t kFrontPorch = 10;
constexpr uint32_t kHzInKHz = 1000;

namespace android::drm_hwcomposer {

void HwcDisplayConfigs::GenFakeMode(uint16_t width, uint16_t height) {
  char value[PROPERTY_VALUE_MAX];
  uint32_t xres = 0, yres = 0;
  if (property_get("debug.drm.mode.force", value, NULL)) {
    // parse <xres>x<yres>[@<refreshrate>]
    if (sscanf(value, "%dx%d", &xres, &yres) != 2) {
      xres = yres = 0;
    }
  }

  const uint16_t kHeadlessModeDisplayWidthMm = xres ? xres : 1600;
  const uint16_t kHeadlessModeDisplayHeightMm = yres ? yres : 900;
  const uint16_t kHeadlessModeDisplayWidthPx = xres ? xres : 1920;
  const uint16_t kHeadlessModeDisplayHeightPx = yres ? yres : 1080;
  ALOGI("set mode %dx%d@%dHz for HEADLESS-MODE", kHeadlessModeDisplayWidthPx,
        kHeadlessModeDisplayHeightPx, kHeadlessModeDisplayVRefresh);

  hwc_configs.clear();

  preferred_config_id = active_config_id = next_config_id++;
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

  hwc_configs[active_config_id] = (HwcDisplayConfig){
      .id = active_config_id,
      .group_id = 1,
      .mode = DrmMode(&headless_drm_mode_info),
      .output_type = OutputType::kSystem,
  };

  mm_width = kHeadlessModeDisplayWidthMm;
  mm_height = kHeadlessModeDisplayHeightMm;
}

bool HwcDisplayConfigs::Init(DrmConnector &connector) {
  // Ensure one config is available for headless mode in case we end up with no
  // real modes from the connector.
  GenFakeMode(0, 0);

  // Probe the connector for modes (IOCTL).
  auto ret = connector.UpdateModes();
  if (ret != 0) {
    ALOGE("Failed to update display modes %d", ret);
    return false;
  }

  if (connector.GetModes().empty()) {
    ALOGE("No modes reported by KMS");
    return false;
  }

  hwc_configs.clear();
  preferred_config_id = 0;
  mm_width = connector.GetMmWidth();
  mm_height = connector.GetMmHeight();

  ConfigId first_config_id = next_config_id;
  uint32_t next_group_id = 1;
  for (const auto &mode : connector.GetModes()) {
    bool disabled = false;
    if ((mode.GetRawMode().flags & DRM_MODE_FLAG_3D_MASK) != 0) {
      ALOGI("Disabling display mode %s (Modes with 3D flag aren't supported)",
            mode.GetName().c_str());
      disabled = true;
    }

    const ConfigId new_config_id = next_config_id++;
    const uint32_t new_group_id = next_group_id++;
    hwc_configs[new_config_id] = {
        .id = new_config_id,
        .group_id = new_group_id,
        .mode = mode,
        .disabled = disabled,
        .output_type = OutputType::kSystem,
    };

    if ((mode.GetRawMode().type & DRM_MODE_TYPE_PREFERRED) != 0 &&
        preferred_config_id == 0) {
      preferred_config_id = new_config_id;
    }
  }

  /* We must have preferred mode. Set first mode as preferred
   * in case KMS haven't reported anything. */
  if (preferred_config_id == 0 && !hwc_configs.empty()) {
    preferred_config_id = first_config_id;
  }

  return true;
}

bool HwcDisplayConfigs::SanitizeGroups() {
  /* A config group should not contain 2 modes with FPS delta less than ~1HZ
   * otherwise android.graphics.cts.SetFrameRateTest CTS will fail
   */
  constexpr float kMinFpsDelta = 1.0;
  for (const auto &[id1, config1] : hwc_configs) {
    for (auto &[id2, config2] : hwc_configs) {
      if (id1 == id2) {
        continue;
      }

      if (config1.group_id != config2.group_id) {
        continue;
      }

      if (config1.disabled || config2.disabled) {
        continue;
      }

      if (fabsf(config1.mode.GetVRefresh() - config2.mode.GetVRefresh()) >=
          kMinFpsDelta) {
        continue;
      }

      ALOGI(
          "Group %i: Disabling display mode %s (Refresh rate value is "
          "too close to existing mode %s)",
          config2.group_id, config2.mode.GetName().c_str(),
          config1.mode.GetName().c_str());

      config2.disabled = true;
    }
  }

  return true;
}

}  // namespace android::drm_hwcomposer
