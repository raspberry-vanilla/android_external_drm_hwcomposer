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

#include "CompositionStats.h"

namespace android::drm_hwcomposer {

bool operator<(const CompositionAttributes& a, const CompositionAttributes& b) {
  return std::make_tuple(a.display_handle, a.present_failed,
                         a.validation_result, a.flatten_reason) <
         std::make_tuple(b.display_handle, b.present_failed,
                         b.validation_result, b.flatten_reason);
}

CompositionStats& CompositionStats::operator+=(const CompositionStats& other) {
  total_frames += other.total_frames;
  total_pixops += other.total_pixops;
  gpu_pixops += other.gpu_pixops;
  failed_kms_validate += other.failed_kms_validate;
  failed_kms_present += other.failed_kms_present;
  frames_flattened += other.frames_flattened;
  cursor_plane_frames += other.cursor_plane_frames;
  failed_kms_cursor_validate += other.failed_kms_cursor_validate;
  layer_count += other.layer_count;
  used_plane_count += other.used_plane_count;
  return *this;
}

CompositionStats operator-(const CompositionStats& a,
                           const CompositionStats& b) {
  return {a.total_frames - b.total_frames,
          a.total_pixops - b.total_pixops,
          a.gpu_pixops - b.gpu_pixops,
          a.failed_kms_validate - b.failed_kms_validate,
          a.failed_kms_present - b.failed_kms_present,
          a.frames_flattened - b.frames_flattened,
          a.cursor_plane_frames - b.cursor_plane_frames,
          a.failed_kms_cursor_validate - b.failed_kms_cursor_validate,
          a.layer_count - b.layer_count,
          a.used_plane_count - b.used_plane_count};
}

void CompositionStatsTracker::ReportStats(const Callback& callback) {
  auto new_stats = provider_->PullCompositionStats();
  for (const auto& [attributes, cumulative_stats] : new_stats) {
    const auto it = previous_stats_.find(attributes);
    const auto prev = it == previous_stats_.end() ? CompositionStats{}
                                                  : it->second;
    const auto delta = cumulative_stats - prev;
    callback(attributes, cumulative_stats, delta);
  }
  previous_stats_ = new_stats;
}

}  // namespace android::drm_hwcomposer
