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

#include "BacklightController.h"

#include <algorithm>

#include "utils/ColorUtil.h"

namespace android::drm_hwcomposer {

const float BacklightController::kMin = 0.0F;
const float BacklightController::kMax = 1.0F;

// The Framework reports brightness values in Linear space (physical
// brightness). Most backlights are passthrough and rely on a non-linear input
// signal to achieve perceptual linearity for the user. This function
// performs the Opto-Electronic Transfer Function (OETF) to convert the
// Linear physical brightness to the HLG signal.
//
// This function is modeled from:
//   com.android.settingslib.display.BrightnessUtils.convertLinearToGamma
auto BacklightController::HlgOetf(float linear) -> float {
  // Normalize to the range [0, 12] rather than [0, 1]
  static const float kScale = 12.0F;
  if (linear < kMin || linear > kMax) {
    return std::clamp(linear, kMin, kMax);
  }
  return static_cast<float>(ColorUtil::EvaluateHlgOetf(linear * kScale));
}

}  // namespace android::drm_hwcomposer
