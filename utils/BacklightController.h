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

#include <optional>

namespace android::drm_hwcomposer {

class BacklightController {
 public:
  static auto HlgOetf(float linear) -> float;
  virtual bool SetBrightness(std::optional<float> brightness) = 0;
  virtual auto GetName() const -> std::string = 0;
  virtual ~BacklightController() = default;

  static const float kMin;
  static const float kMax;
};

}  // namespace android::drm_hwcomposer
