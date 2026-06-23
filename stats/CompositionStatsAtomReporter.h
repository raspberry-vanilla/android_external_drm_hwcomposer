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

#include <cstdint>
#include <memory>

#include "compositor/CompositionPlanner.h"
#include "stats/Stats.h"

namespace android::drm_hwcomposer {

// CompositionStatsAtomReporter is a wrapper around creation of a VendorAtom
// and pushing it to the IStats::reportVendorAtom interface.
class CompositionStatsAtomReporter {
 public:
  // Returns nullptr if atom reporting is not enabled via soong config variables
  // or if there is some error getting the IStats service.
  static std::unique_ptr<CompositionStatsAtomReporter> Create();

  // Pushes a Vendor Atom to IStats::reportVendorAtom.
  virtual void PushAtom(int64_t display_handle, bool present_failed,
                        int32_t present_error_code,
                        ValidationResult validation_result,
                        int32_t validation_error_code,
                        CompositionPlanner::FlattenReason flatten_reason,
                        int64_t frame_count, int64_t layer_count,
                        int64_t used_plane_count, uint64_t total_pixops,
                        uint64_t gpu_pixops) = 0;
  virtual ~CompositionStatsAtomReporter() = default;
};

}  // namespace android::drm_hwcomposer
