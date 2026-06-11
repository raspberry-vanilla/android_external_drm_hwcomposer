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

#pragma once

#include <drm/drm_mode.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ui {
enum class Hdr;
}  // namespace ui

namespace android::drm_hwcomposer {
class DisplayHotplugConnectModeDetectedAtomReporter {
 public:
  static std::unique_ptr<DisplayHotplugConnectModeDetectedAtomReporter>
  Create();

  enum class DisplayType {
    kUnspecified = 0,
    kInternal,
    kExternal,
  };

  struct Atom {
    int64_t display_handle = 0;
    int32_t resolution_x = 0;
    int32_t resolution_y = 0;
    int32_t refresh_rate = 0;
    int32_t dpi_x = 0;
    int32_t dpi_y = 0;
    DisplayType display_type = DisplayType::kUnspecified;
    bool is_preferred = false;
    std::string make;
    std::string model;
    int32_t year = 0;
    std::vector<ui::Hdr> hdr_types;
    float max_luminance = 0.0F;
    float max_average_luminance = 0.0F;
    float min_luminance = 0.0F;
    uint32_t connection_type = DRM_MODE_CONNECTOR_Unknown;
    bool has_path = false;
    uint32_t vrr_range_min = 0;
    uint32_t vrr_range_max = 0;

    bool operator==(const Atom& other) const {
      return display_handle == other.display_handle &&
             resolution_x == other.resolution_x &&
             resolution_y == other.resolution_y &&
             refresh_rate == other.refresh_rate && dpi_x == other.dpi_x &&
             dpi_y == other.dpi_y && display_type == other.display_type &&
             is_preferred == other.is_preferred && make == other.make &&
             model == other.model && year == other.year &&
             hdr_types == other.hdr_types &&
             max_luminance == other.max_luminance &&
             max_average_luminance == other.max_average_luminance &&
             min_luminance == other.min_luminance &&
             connection_type == other.connection_type &&
             has_path == other.has_path &&
             vrr_range_min == other.vrr_range_min &&
             vrr_range_max == other.vrr_range_max;
    };
  };

  // Pushes a Vendor Atom to IStats::reportVendorAtom.
  virtual void PushAtom(Atom atom) = 0;
  virtual ~DisplayHotplugConnectModeDetectedAtomReporter() = default;
};

}  // namespace android::drm_hwcomposer
