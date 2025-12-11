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

namespace android::drm_hwcomposer {

class CountActiveDisplaysReporter {
 public:
  static std::unique_ptr<CountActiveDisplaysReporter> Create();

  // Pushes a Vendor Atom to IStats::reportVendorAtom.
  virtual void PushAtom(int32_t num_active_physical_displays,
                        int32_t num_active_external_displays,
                        int32_t num_virtual_displays) = 0;
  virtual ~CountActiveDisplaysReporter() = default;
};

}  // namespace android::drm_hwcomposer
