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

#include "drm/DrmProperty.h"

#include <gtest/gtest.h>

#include <drm/drm_mode.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace android::drm_hwcomposer {

TEST(DrmPropertyTest, EnumPropertyGetEnumValuesDoesNotPrependZeros) {
  constexpr uint32_t kEnumObjId = 500;
  constexpr uint32_t kEnumPropId = 501;
  constexpr uint64_t kColorOpEnumVal = 39;
  drmModePropertyRes raw_prop{};
  raw_prop.prop_id = kEnumPropId;
  raw_prop.flags = DRM_MODE_PROP_ENUM;
  std::strncpy(raw_prop.name, "COLOR_PIPELINE", DRM_PROP_NAME_LEN - 1);
  uint64_t values[2] = {0, kColorOpEnumVal};
  raw_prop.count_values = 2;
  raw_prop.values = values;
  drm_mode_property_enum enums[2] = {{0, "Zero"},
                                     {kColorOpEnumVal, "Color Pipeline 39"}};
  raw_prop.count_enums = 2;
  raw_prop.enums = enums;

  DrmProperty prop(nullptr, kEnumObjId, &raw_prop, 0);
  const std::vector<uint64_t> expected_values = {0, kColorOpEnumVal};
  EXPECT_EQ(prop.GetEnumValues(), expected_values);
}

TEST(DrmPropertyTest, EnumPropertyGetValueMatchesRawKernelEnumValue) {
  constexpr uint32_t kEnumObjId = 500;
  constexpr uint32_t kEnumPropId = 501;
  constexpr uint64_t kColorOpEnumVal = 39;
  drmModePropertyRes raw_prop{};
  raw_prop.prop_id = kEnumPropId;
  raw_prop.flags = DRM_MODE_PROP_ENUM;
  std::strncpy(raw_prop.name, "COLOR_PIPELINE", DRM_PROP_NAME_LEN - 1);
  uint64_t values[2] = {0, kColorOpEnumVal};
  raw_prop.count_values = 2;
  raw_prop.values = values;
  drm_mode_property_enum enums[2] = {{0, "Zero"},
                                     {kColorOpEnumVal, "Color Pipeline 39"}};
  raw_prop.count_enums = 2;
  raw_prop.enums = enums;

  DrmProperty prop(nullptr, kEnumObjId, &raw_prop, kColorOpEnumVal);
  EXPECT_EQ(prop.GetValue(), kColorOpEnumVal);
}

}  // namespace android::drm_hwcomposer
