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
#pragma once

#include <chrono>
#include <cstddef>
#include <deque>

namespace android::drm_hwcomposer {

// Tracks recent frame update history for a layer to produce an approximate FPS
// for a given point in time.
class FrameTimeHistory {
 public:
  using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

  // Track a new frame update that happened around |time|.
  void AddFrameTime(TimePoint time = std::chrono::steady_clock::now());

  // Calculate the approximate FPS around |now|. Infrequnetly updated layers
  // will return 0.0 FPS. Only valid around |kMaxFrameTimeAge| before the last
  // frame update.
  float CalculateFps(
      TimePoint now = std::chrono::steady_clock::now()) const;

  static constexpr std::chrono::milliseconds
      kMaxFrameTimeAge = std::chrono::milliseconds(1200);

 private:
  // Sorted list of recent frame updates from the least to most recent.
  std::deque<TimePoint> frame_times_;
  static constexpr size_t kFrameTimesSize = 30;

  // A layer is only "frequent" if it has had a certain number of updates within
  // a given |kMaxFrameTimeAge| window. Otherwise default to 0fps.
  static constexpr int kFrequentLayerWindowSize = 3;
};

}  // namespace android::drm_hwcomposer