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

#include <cstdint>
#include <map>
#include <optional>

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
  OutputType output_type{};
};

struct HwcDisplayConfigs {
  // Removes problematic configs from groups after they were set.
  bool SanitizeGroups();

  std::map<ConfigId, struct HwcDisplayConfig> hwc_configs;

  ConfigId preferred_config_id = 0;

  uint32_t mm_width = 0;
  uint32_t mm_height = 0;
};

class BackendDisplayCapabilities;

struct HwcConfigParameters {
  bool use_color_pipeline = false;
  bool persistent_hdr_enabled = false;
  bool external_hdr_enabled = false;
  const BackendDisplayCapabilities *capabilities = nullptr;
  std::optional<float> min_refresh_rate = std::nullopt;
  std::optional<float> max_refresh_rate = std::nullopt;
  bool force_disable_mrr = false;
};

class HwcDisplayConfigsGenerator {
 public:
  HwcDisplayConfigsGenerator() = default;

  std::optional<HwcDisplayConfigs> GenerateDisplayConfigs(
      const DrmConnector &connector, const HwcConfigParameters &params);
  HwcDisplayConfigs GetFakeMode(uint16_t width, uint16_t height);

 private:
  // Use sequential config IDs throughout the lifetime of the owner display to
  // prevent race conditions around hotplugs (mode updates). See:
  // https://source.android.com/docs/core/graphics/hotplug#prevent-race-conditions
  ConfigId next_config_id_ = 1;
};

}  // namespace android::drm_hwcomposer
