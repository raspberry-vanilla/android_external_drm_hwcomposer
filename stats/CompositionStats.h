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

namespace android {

struct CompositionStats {
  uint32_t total_frames = 0;
  uint64_t total_pixops = 0;
  uint64_t gpu_pixops = 0;
  uint32_t failed_kms_validate = 0;
  uint32_t failed_kms_present = 0;
  uint32_t frames_flattened = 0;
  uint32_t cursor_plane_frames = 0;
  uint32_t failed_kms_cursor_validate = 0;

  // When adding new stats, update the operator- below as well as
  // operator== and operator<< which are implemented in the unit test file.
};

// Used for calculating the delta between two CompositionStats.
CompositionStats operator-(const CompositionStats& a,
                           const CompositionStats& b);

// Interface for a reporter of per-display CompositionStats.
class CompositionStatsProvider {
 public:
  // Get cumulative stats per display.
  virtual auto PullCompositionStats()
      -> std::map<int64_t, CompositionStats> = 0;
  virtual ~CompositionStatsProvider() = default;
};

// CompositionStatsTracker pulls stats from a CompositionStatsProvider on-demand
// and keeps track of the previous stats state in order to calculate the deltas.
class CompositionStatsTracker {
 public:
  // Arguments are the display ID, the cumulative stats, and the stats delta.
  using Callback = std::function<void(int64_t display_id,
                                      const CompositionStats& cumulative,
                                      const CompositionStats& delta)>;
  explicit CompositionStatsTracker(CompositionStatsProvider* provider)
      : provider_(provider) {
  }

  // Callback will be called for each display, with the cumulative
  // stats and the stats delta from the previous invocation.
  void ReportStats(const Callback& callback);

 private:
  CompositionStatsProvider* provider_;
  std::map<int64_t, CompositionStats> previous_stats_;
};

}  // namespace android
