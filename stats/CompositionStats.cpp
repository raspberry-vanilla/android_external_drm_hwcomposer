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

#include "stats/CompositionStats.h"

namespace android::drm_hwcomposer {

CompositionStats operator-(const CompositionStats& a,
                           const CompositionStats& b) {
  return {a.total_frames - b.total_frames,
          a.total_pixops - b.total_pixops,
          a.gpu_pixops - b.gpu_pixops,
          a.failed_kms_validate - b.failed_kms_validate,
          a.failed_kms_present - b.failed_kms_present,
          a.frames_flattened - b.frames_flattened,
          a.cursor_plane_frames - b.cursor_plane_frames,
          a.failed_kms_cursor_validate - b.failed_kms_cursor_validate};
}

void CompositionStatsTracker::ReportStats(const Callback& callback) {
  auto new_stats = provider_->PullCompositionStats();
  for (auto& [display_handle, cumulative_stats] : new_stats) {
    auto delta = cumulative_stats - previous_stats_[display_handle];
    callback(display_handle, cumulative_stats, delta);
  }
  previous_stats_ = new_stats;
}

}  // namespace android::drm_hwcomposer
