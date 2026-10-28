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

#include "utils/TestUtils.h"

#include <gtest/gtest.h>

#include "utils/properties.h"

namespace android::drm_hwcomposer {

TEST(TestUtilsTest, ScopedTestPropertyRestoresPreviousValueOnDestruction) {
  const ScopedTestProperty outer_prop("vendor.hwc.drm.scale_with_gpu", "1");
  ASSERT_TRUE(Properties::ScaleWithGpu());
  {
    const ScopedTestProperty inner_prop("vendor.hwc.drm.scale_with_gpu", "0");
    EXPECT_FALSE(Properties::ScaleWithGpu());
  }
  EXPECT_TRUE(Properties::ScaleWithGpu());
}

TEST(TestUtilsTest, ScopedTestPropertyClearsUnsetPropertyOnDestruction) {
  const ScopedTestProperty baseline_prop("vendor.hwc.drm.device", nullptr);
  ASSERT_TRUE(Properties::GetDevicePath().empty());
  {
    const ScopedTestProperty override_prop("vendor.hwc.drm.device",
                                           "/dev/dri/card9");
    EXPECT_EQ(Properties::GetDevicePath(), "/dev/dri/card9");
  }
  EXPECT_TRUE(Properties::GetDevicePath().empty());
}

}  // namespace android::drm_hwcomposer
