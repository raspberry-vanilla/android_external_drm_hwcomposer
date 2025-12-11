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
#include <functional>
#include <map>

#include "compositor/CompositionPlanner.h"

namespace android::drm_hwcomposer {

enum class ValidationResult {
  kSuccess,
  kFailure,
  kSkip,
};

struct CompositionAttributes {
  int64_t display_handle = 0;
  bool present_failed = false;
  ValidationResult validation_result = ValidationResult::kSkip;
  CompositionPlanner::FlattenReason
      flatten_reason = CompositionPlanner::FlattenReason::kNone;

  // When adding new attributes, update the operator< below as well as
  // operator== which is implemented in the unit test file.
};

bool operator<(const CompositionAttributes& a, const CompositionAttributes& b);

struct CompositionStats {
  uint32_t total_frames = 0;
  uint64_t total_pixops = 0;
  uint64_t gpu_pixops = 0;
  uint32_t failed_kms_validate = 0;
  uint32_t failed_kms_present = 0;
  uint32_t frames_flattened = 0;
  uint32_t cursor_plane_frames = 0;
  uint32_t failed_kms_cursor_validate = 0;
  uint32_t layer_count = 0;
  uint32_t used_plane_count = 0;

  // When adding new stats, update the operator+= and operator- below as well as
  // operator== and operator<< which are implemented in the unit test file.

  CompositionStats& operator+=(const CompositionStats& other);
};

// Used for calculating the delta between two CompositionStats.
CompositionStats operator-(const CompositionStats& a,
                           const CompositionStats& b);

struct ActiveDisplayCounts {
  int32_t num_active_physical_displays = 0;
  int32_t num_active_external_displays = 0;
  int32_t num_virtual_displays = 0;
};

class StatsProvider {
 public:
  // Get cumulative stats per unique attributes.
  virtual auto PullCompositionStats()
      -> std::map<CompositionAttributes, CompositionStats> = 0;

  virtual auto PullActiveDisplayCounts() -> ActiveDisplayCounts = 0;
  virtual ~StatsProvider() = default;
};

// StatsTracker pulls stats from a StatsProvider on-demand and keeps track of
// the previous stats state in order to calculate the deltas if necessary.
class StatsTracker {
 public:
  // Arguments are the attributes, the cumulative stats, and the stats delta.
  using Callback = std::function<void(const CompositionAttributes& attributes,
                                      const CompositionStats& cumulative,
                                      const CompositionStats& delta)>;
  explicit StatsTracker(StatsProvider* provider) : provider_(provider) {
  }

  // Callback will be called for each unique attribute (empty entries are
  // skipped), with the cumulative stats and the stats delta from the previous
  // invocation.
  void ReportCompositionStats(const Callback& callback);

  ActiveDisplayCounts CountActiveDisplays();

 private:
  StatsProvider* provider_;
  std::map<CompositionAttributes, CompositionStats> previous_stats_;
};

}  // namespace android::drm_hwcomposer
