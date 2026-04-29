/*
 * Copyright (C) 2026 The Android Open Source Project
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
#include "compositor/FrameTimeHistory.h"

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace android::drm_hwcomposer {

void FrameTimeHistory::AddFrameTime(const TimePoint time) {
  frame_times_.push_back(time);

  // Sort from least to most recent.
  std::sort(frame_times_.begin(), frame_times_.end());

  // |frame_times_| can only have |kFrameTimesSize| frame times and the max age
  // difference can only be |kMaxFrameTimeAge|.
  while (!frame_times_.empty() &&
         (frame_times_.back() - frame_times_.front() > kMaxFrameTimeAge ||
          frame_times_.size() >= kFrameTimesSize)) {
    frame_times_.pop_front();
  }
}

float FrameTimeHistory::CalculateFps(const TimePoint now) const {
  int earliest_active_index = 0;
  for (; static_cast<size_t>(earliest_active_index) < frame_times_.size();
       earliest_active_index++) {
    if (now - frame_times_[earliest_active_index] < kMaxFrameTimeAge) {
      break;
    }
  }

  int latest_active_index = static_cast<int>(frame_times_.size()) - 1;
  for (; latest_active_index >= 0; latest_active_index--) {
    if (frame_times_[latest_active_index] <= now) {
      break;
    }
  }

  const int num_active_frames = latest_active_index - earliest_active_index;
  if (num_active_frames < kFrequentLayerWindowSize) {
    return 0.0F;
  }

  const auto
      active_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               frame_times_[latest_active_index] -
                               frame_times_[earliest_active_index])
                               .count();
  constexpr float kOneSecNs = 1000000000.0F;
  return static_cast<float>(num_active_frames) * kOneSecNs /
         static_cast<float>(active_duration_ns);
}

};  // namespace android::drm_hwcomposer
