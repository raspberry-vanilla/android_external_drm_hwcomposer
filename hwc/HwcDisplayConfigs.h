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

#pragma once

#include <map>

#include "drm/DrmMode.h"

namespace android::drm_hwcomposer {

using ConfigId = int32_t;

class DrmConnector;

/**
 * Display panel colorspace property values.
 */
enum class OutputType : uint32_t {
  kInvalid,
  kSystem,
  kSdr,
  kHdr10,
};

struct HwcDisplayConfig {
  ConfigId id{};
  uint32_t group_id{};
  DrmMode mode{};
  bool disabled{};
  OutputType output_type{};

  bool IsInterlaced() const {
    return (mode.GetRawMode().flags & DRM_MODE_FLAG_INTERLACE) != 0;
  }
};

struct HwcDisplayConfigs {
  bool Init(DrmConnector &connector);
  void GenFakeMode(uint16_t width, uint16_t height);

  // Removes problematic configs from groups after they were set.
  bool SanitizeGroups();

  std::map<ConfigId, struct HwcDisplayConfig> hwc_configs;

  ConfigId active_config_id = 0;
  ConfigId preferred_config_id = 0;

  // Use sequential config IDs throughout the lifetime of the owner display to
  // prevent race conditions around hotplugs (mode updates). See:
  // https://source.android.com/docs/core/graphics/hotplug#prevent-race-conditions
  ConfigId next_config_id = 1;

  uint32_t mm_width = 0;
  uint32_t mm_height = 0;
};

}  // namespace android::drm_hwcomposer
