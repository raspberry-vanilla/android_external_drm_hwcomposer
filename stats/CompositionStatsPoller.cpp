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

#include "CompositionStatsPoller.h"

#include <chrono>

#include "hwc/HwcDisplay.h"
#include "stats/CompositionStatsAtomReporter.h"

namespace android::drm_hwcomposer {

CompositionStatsPoller::CompositionStatsPoller(
    std::unique_ptr<CompositionStatsAtomReporter> reporter,
    CompositionStatsProvider* provider)
    : tracker_(provider), reporter_(std::move(reporter)) {
  thread_ = std::thread(&CompositionStatsPoller::PollFunc, this);
}

CompositionStatsPoller::~CompositionStatsPoller() {
  {
    std::lock_guard lock(mutex_);
    exit_ = true;
  }
  condition_.notify_one();
  thread_.join();
}

void CompositionStatsPoller::PollFunc() {
  bool thread_exit = false;
  while (!thread_exit) {
    tracker_.ReportStats([this](DisplayHandle display_handle,
                                const CompositionStats& /*cumulative*/,
                                const CompositionStats& delta) {
      if (delta.total_frames == 0) {
        return;
      }
      reporter_->PushAtom(display_handle, delta.total_frames,
                          delta.failed_kms_present, delta.failed_kms_validate);
    });

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
