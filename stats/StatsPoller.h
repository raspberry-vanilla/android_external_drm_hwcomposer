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

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <android-base/thread_annotations.h>

#include "stats/Stats.h"

namespace android::drm_hwcomposer {

class CompositionStatsAtomReporter;
class CountActiveDisplaysReporter;

// StatsPoller periodically polls the StatsProvider for various
// stats. It then pushes the stats delta to the CompositionStatsAtomReporter.
class StatsPoller {
 public:
  StatsPoller(std::unique_ptr<CompositionStatsAtomReporter> stats_reporter,
              std::unique_ptr<CountActiveDisplaysReporter>
                  count_active_displays_reporter,
              StatsProvider* provider);
  ~StatsPoller();

 private:
  void PollFunc();

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable condition_;

  // Accessed from both the helper thread and main thread.
  bool exit_ GUARDED_BY(mutex_) = false;

  // Only accessed from the helper thread.
  StatsTracker tracker_;
  std::unique_ptr<CompositionStatsAtomReporter> stats_reporter_;
  std::unique_ptr<CountActiveDisplaysReporter> count_active_displays_reporter_;
};

}  // namespace android::drm_hwcomposer
