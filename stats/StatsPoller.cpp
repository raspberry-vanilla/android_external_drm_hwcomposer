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

#include "StatsPoller.h"

#include <android-base/thread_annotations.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

#include "stats/CompositionStatsAtomReporter.h"
#include "stats/CountActiveDisplaysReporter.h"
#include "stats/Stats.h"

namespace android::drm_hwcomposer {

StatsPoller::StatsPoller(
    std::unique_ptr<CompositionStatsAtomReporter> stats_reporter,
    std::unique_ptr<CountActiveDisplaysReporter> count_active_displays_reporter,
    StatsProvider* provider)
    : tracker_(provider),
      stats_reporter_(std::move(stats_reporter)),
      count_active_displays_reporter_(
          std::move(count_active_displays_reporter)) {
  thread_ = std::thread(&StatsPoller::PollFunc, this);
}

StatsPoller::~StatsPoller() {
  {
    std::lock_guard lock(mutex_);
    exit_ = true;
  }
  condition_.notify_one();
  thread_.join();
}

void StatsPoller::PollFunc() {
  bool thread_exit = false;
  while (!thread_exit) {
    tracker_.ReportCompositionStats(
        [this](const CompositionAttributes& attributes,
               const CompositionStats& /*cumulative*/,
               const CompositionStats& delta) {
          if (delta.total_frames == 0) {
            return;
          }
          stats_reporter_->PushAtom(attributes.display_handle,
                                    attributes.present_failed,
                                    attributes.present_error_code,
                                    attributes.validation_result,
                                    attributes.validation_error_code,
                                    attributes.flatten_reason,
                                    delta.total_frames, delta.layer_count,
                                    delta.used_plane_count, delta.total_pixops,
                                    delta.gpu_pixops);
        });

    const ActiveDisplayCounts
        active_display_counts = tracker_.CountActiveDisplays();
    count_active_displays_reporter_
        ->PushAtom(active_display_counts.num_active_physical_displays,
                   active_display_counts.num_active_external_displays,
                   active_display_counts.num_virtual_displays);

    constexpr std::chrono::seconds kPollFrequency = std::chrono::minutes(1);
    std::unique_lock lock(mutex_);
    base::ScopedLockAssertion lock_assertion(mutex_);
    thread_exit = exit_ ||
                  condition_.wait_for(lock, kPollFrequency, [this]() -> bool {
                    base::ScopedLockAssertion lock_assertion(mutex_);
                    return exit_;
                  });
  }
}

}  // namespace android::drm_hwcomposer
