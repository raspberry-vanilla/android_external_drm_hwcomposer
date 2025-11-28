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

#include <memory>
#include <vector>

namespace android::drm_hwcomposer {
class DisplayRefreshRatesChangedAtomReporter {
 public:
  static std::unique_ptr<DisplayRefreshRatesChangedAtomReporter> Create();

  virtual void UpdateRefreshRates(std::vector<int32_t> refresh_rates) = 0;
  virtual ~DisplayRefreshRatesChangedAtomReporter() = default;
};

}  // namespace android::drm_hwcomposer
