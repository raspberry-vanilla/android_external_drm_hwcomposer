/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "HdcpController.h"

#include <android-base/thread_annotations.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

#include "compositor/DisplayInfo.h"
#include "drm/DrmConnector.h"
#include "drm/DrmDisplayPipeline.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

namespace {
constexpr auto kEarlyCheckInterval = std::chrono::milliseconds(100);
}  // namespace

HdcpController::HdcpController(const DrmDisplayPipeline* pipeline,
                               HdcpConCallbacks callbacks,
                               std::chrono::milliseconds timeout)
    : pipeline_(pipeline), cbks_(std::move(callbacks)), timeout_(timeout) {
  thread_ = std::thread(&HdcpController::ThreadFn, this);
}

HdcpController::~HdcpController() {
  {
    auto lock = std::lock_guard<std::mutex>(mutex_);
    hdcp_state_ = HdcpState::kThreadExit;
    cv_.notify_all();
  }
  thread_.join();
}

// Set the HDCP state to desired if not already requested
void HdcpController::Start() {
  bool changed = false;
  bool should_notify = false;
  std::optional<HdcpContentType> notify_type;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (hdcp_state_) {
      case HdcpState::kEnabled:
        should_notify = true;
        notify_type = enabled_content_type_;
        break;
      case HdcpState::kInfeasible:
        should_notify = true;
        notify_type = std::nullopt;
        break;
      case HdcpState::kRetry:
        break;
      default:
        ALOGI("HDCP requested to start");
        hdcp_state_ = HdcpState::kDesired;
        enabled_content_type_ = std::nullopt;
        changed = true;
        break;
    }
  }

  if (should_notify) {
    cbks_.notify_hdcp_status(notify_type);
  }

  if (changed) {
    cv_.notify_all();
  }
}

void HdcpController::Requested() {
  auto lock = std::lock_guard<std::mutex>(mutex_);

  if (hdcp_state_ == HdcpState::kEnabled ||
      hdcp_state_ == HdcpState::kUndesired ||
      hdcp_state_ == HdcpState::kInfeasible ||
      hdcp_state_ == HdcpState::kThreadExit) {
    return;
  }

  ++request_generation_;
  was_retry_ = (hdcp_state_ == HdcpState::kRetry);
  hdcp_state_ = HdcpState::kRequested;

  sleep_until_ = std::chrono::system_clock::now() + timeout_;
  cv_.notify_all();
}

void HdcpController::SetContentProtectionStatus(uint64_t expected_generation,
                                                bool only_if_enabled) {
  auto* connector = pipeline_->connector->Get();

  // Use QueryContentProtectionEnabled() instead of UpdateContentProtection()
  // followed by IsContentProtectionEnabled() to avoid modifying shared cached
  // state from the HDCP worker thread. Query outside the controller lock to
  // avoid blocking while holding the mutex.
  const bool cp_enabled = connector->QueryContentProtectionEnabled();
  if (!cp_enabled && only_if_enabled) {
    return;
  }

  // Decide new state and which callbacks to invoke, updating internal state
  // while holding the lock. Do not call callbacks while holding the lock.
  std::optional<HdcpContentType> notify_type;
  bool trigger_retry = false;

  // Verify the request generation still matches so an unlocked connector query
  // from a detached/terminated request cannot mutate a newer client request.
  bool should_notify_hdcp_status = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hdcp_state_ == HdcpState::kRequested &&
        request_generation_ == expected_generation) {
      if (cp_enabled) {
        hdcp_state_ = HdcpState::kEnabled;
        should_notify_hdcp_status = true;
        if (was_retry_) {
          was_retry_ = false;
          notify_type = HdcpContentType::kType0;
        } else {
          notify_type = HdcpContentType::kType1;
        }
        enabled_content_type_ = notify_type;
      } else {
        if (was_retry_) {
          was_retry_ = false;
          hdcp_state_ = HdcpState::kInfeasible;
          notify_type = std::nullopt;
          ALOGI("HDCP negotiation failed after retry. Marking as infeasible.");
          should_notify_hdcp_status = true;
        } else {
          hdcp_state_ = HdcpState::kRetry;
          trigger_retry = true;
        }
      }
    }
  }

  // Invoke callbacks without holding the controller mutex.
  if (should_notify_hdcp_status) {
    cbks_.notify_hdcp_status(notify_type);
  }
  if (trigger_retry) {
    cbks_.trigger_retry_frame();
  }
}

HdcpController::HdcpState HdcpController::GetHdcpState() const {
  auto lock = std::lock_guard<std::mutex>(mutex_);
  return hdcp_state_;
}

bool HdcpController::GetSettledNotification(
    std::optional<HdcpContentType>* out_level) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (hdcp_state_ == HdcpState::kEnabled) {
    *out_level = enabled_content_type_;
    return true;
  }
  if (hdcp_state_ == HdcpState::kInfeasible) {
    *out_level = std::nullopt;
    return true;
  }
  return false;
}

void HdcpController::Terminate() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hdcp_state_ == HdcpState::kThreadExit) {
      return;
    }
    ++request_generation_;
    hdcp_state_ = HdcpState::kUndesired;
    was_retry_ = false;
    enabled_content_type_ = std::nullopt;
  }
  cbks_.notify_hdcp_status(std::nullopt);
  cv_.notify_all();
}

void HdcpController::Reset() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Preserve kInfeasible across client resets: infeasibility reflects a
    // physical sink/link limitation and only resets when the pipeline is torn
    // down on display disconnect.
    if (hdcp_state_ == HdcpState::kThreadExit ||
        hdcp_state_ == HdcpState::kInfeasible) {
      return;
    }
    ++request_generation_;
    hdcp_state_ = HdcpState::kUndesired;
    was_retry_ = false;
    enabled_content_type_ = std::nullopt;
  }
  cv_.notify_all();
}

void HdcpController::ExpireRequestTimeoutForTesting() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sleep_until_ = std::chrono::system_clock::now();
  }
  cv_.notify_all();
}

void HdcpController::ThreadFn() {
  for (;;) {
    bool fire_callback = false;
    bool early_check = false;
    uint64_t expected_generation = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      base::ScopedLockAssertion lock_assertion(mutex_);

      if (hdcp_state_ == HdcpState::kThreadExit) {
        break;
      }

      auto now = std::chrono::system_clock::now();
      if (hdcp_state_ == HdcpState::kRequested) {
        if (sleep_until_ <= now) {
          // If the timeout has expired, we will fire the callback to check the
          // HDCP status.
          fire_callback = true;
          expected_generation = request_generation_;
        } else {
          ALOGV("Wait_until");
          // Wait for kEarlyCheckInterval or until sleep_until_, whichever is earlier,
          // to re-check the HDCP status.
          // This is to handle the case where HDCP status change times are
          // variable and can sometimes take a long time, so we want to re-check
          // the status periodically until the timeout expires.
          cv_.wait_until(lock,
                         std::min(sleep_until_, now + kEarlyCheckInterval));
          // hdcp_state_ could have been changed while waiting, so check the
          // hdcp_state_ again.
          if (hdcp_state_ == HdcpState::kRequested) {
            early_check = true;
            expected_generation = request_generation_;
          }
        }
      } else {
        ALOGV("Wait");
        cv_.wait(lock);
      }
    }

    // If it's time to check the HDCP status, query the connector and call
    // appropriate callbacks based on the current state and connector status.
    // This needs to happen outside of the lock to avoid blocking other
    // operations while potentially performing I/O or other long-running tasks.
    if (early_check) {
      SetContentProtectionStatus(expected_generation, /*only_if_enabled=*/true);
    } else if (fire_callback) {
      // Call the function that checks the CP prop value and call appropriate
      // onHdcPlevelsChanged
      ALOGV("Timeout. Sending an event to compositor");
      SetContentProtectionStatus(expected_generation,
                                 /*only_if_enabled=*/false);
    }
  }
}

}  // namespace android::drm_hwcomposer
