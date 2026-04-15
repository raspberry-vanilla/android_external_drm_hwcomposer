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

#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

#include "compositor/DisplayInfo.h"
#include "drm/DrmConnector.h"
#include "drm/DrmDisplayPipeline.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hdcp_state_ != HdcpState::kRetry) {
      hdcp_state_ = HdcpState::kDesired;
      changed = true;
    }
  }

  if (changed) {
    cv_.notify_all();
  }
}

void HdcpController::Requested() {
  auto lock = std::lock_guard<std::mutex>(mutex_);

  if (hdcp_state_ == HdcpState::kEnabled ||
      hdcp_state_ == HdcpState::kUndesired) {
    return;
  }

  was_retry_ = (hdcp_state_ == HdcpState::kRetry);
  hdcp_state_ = HdcpState::kRequested;

  sleep_until_ = std::chrono::system_clock::now() + timeout_;
  cv_.notify_all();
}

void HdcpController::SetContentProtectionStatus() {
  auto* connector = pipeline_->connector->Get();

  // Query connector outside of the lock to avoid blocking while holding the
  // controller mutex.
  const bool cp_enabled = connector->IsContentProtectionEnabled();

  // Decide new state and which callbacks to invoke, updating internal state
  // while holding the lock. Do not call callbacks while holding the lock.
  std::optional<HdcpContentType> notify_type;
  bool trigger_retry = false;

  // check if we're in the requested state and capture transient
  // variables under lock. We will release the lock to query the connector
  // and call callbacks to avoid holding the mutex during potentially
  // blocking operations.
  bool should_notify_hdcp_status = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hdcp_state_ == HdcpState::kRequested) {
      if (cp_enabled) {
        hdcp_state_ = HdcpState::kEnabled;
        should_notify_hdcp_status = true;
        if (was_retry_) {
          was_retry_ = false;
          notify_type = HdcpContentType::kType0;
        } else {
          notify_type = HdcpContentType::kType1;
        }
      } else {
        if (was_retry_) {
          was_retry_ = false;
          hdcp_state_ = HdcpState::kUndesired;
          notify_type = std::nullopt;
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

void HdcpController::Terminate() {
  auto lock = std::lock_guard<std::mutex>(mutex_);
  hdcp_state_ = HdcpState::kUndesired;
  cbks_.notify_hdcp_status(std::nullopt);
  cv_.notify_all();
}

void HdcpController::ThreadFn() {
  for (;;) {
    bool fire_callback = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      base::ScopedLockAssertion lock_assertion(mutex_);

      if (hdcp_state_ == HdcpState::kThreadExit) {
        break;
      }

      if (sleep_until_ <= std::chrono::system_clock::now() &&
          (hdcp_state_ == HdcpState::kRequested)) {
        fire_callback = true;
      } else {
        if (hdcp_state_ != HdcpState::kRequested) {
          ALOGV("Wait");
          cv_.wait(lock);
        } else {
          ALOGV("Wait_until");
          cv_.wait_until(lock, sleep_until_);
        }
      }
    }

    if (fire_callback) {
      // Call the function that checks the CP prop value and call appropriate
      // onHdcPlevelsChanged
      ALOGV("Timeout. Sending an event to compositor");
      SetContentProtectionStatus();
    }
  }
}

}  // namespace android::drm_hwcomposer