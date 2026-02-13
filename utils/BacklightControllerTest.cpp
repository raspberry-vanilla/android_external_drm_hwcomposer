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

#include <gtest/gtest.h>

#include "BacklightController.h"

namespace android::drm_hwcomposer {

// Test that a zero brightness value maps to a zero HLG signal.
TEST(BacklightControllerTest, TestZeroInput) {
  EXPECT_FLOAT_EQ(BacklightController::HlgOetf(0.0F), 0.0F);
}

// Test that the OETF is correct across a range of values.
TEST(BacklightControllerTest, TestLUT) {
  static const std::map<float, float> kBrightnessLookupTable = {
      {0.01F, 0.1732F}, {0.02F, 0.2449F}, {0.03F, 0.3000F}, {0.04F, 0.3464F},
      {0.05F, 0.3873F}, {0.1F, 0.5444F},  {0.2F, 0.6939F},  {0.3F, 0.7743F},
      {0.4F, 0.8295F},  {0.5F, 0.8716F},  {0.6F, 0.9057F},  {0.7F, 0.9343F},
      {0.8F, 0.9590F},  {0.9F, 0.9807F},
  };
  for (const auto &entry : kBrightnessLookupTable) {
    EXPECT_NEAR(BacklightController::HlgOetf(entry.first), entry.second,
                0.0005F);
  }
}

// Test that a unit brightness value maps to a unit HLG signal.
TEST(BacklightControllerTest, TestOneInput) {
  EXPECT_FLOAT_EQ(BacklightController::HlgOetf(1.0F), 1.0F);
}

// Test that a brightness value less than zero maps to a zero HLG signal.
TEST(BacklightControllerTest, TestNegativeInput) {
  EXPECT_FLOAT_EQ(BacklightController::HlgOetf(-0.5F), 0.0F);
}

// Test that a brightness value greater than one maps to a unit HLG signal.
TEST(BacklightControllerTest, TestOverflowInput) {
  EXPECT_FLOAT_EQ(BacklightController::HlgOetf(5.0F), 1.0F);
}

}  // namespace android::drm_hwcomposer
