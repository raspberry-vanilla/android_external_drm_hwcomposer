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

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include <android-base/thread_annotations.h>

#include "compositor/DisplayInfo.h"
#include "drm/DrmDisplayPipeline.h"

namespace android::drm_hwcomposer {

struct DrmDisplayPipeline;

// NOLINTNEXTLINE(misc-unused-using-decls): False positive
using std::chrono_literals::operator""s;

struct HdcpConCallbacks {
  std::function<void(std::optional<HdcpContentType>)> notify_hdcp_status;
  std::function<void()> trigger_retry_frame;
};

class HdcpController {
 public:
  HdcpController(const DrmDisplayPipeline* pipeline, HdcpConCallbacks callbacks,
                 std::chrono::milliseconds hdcp_enabled_timeout);
  ~HdcpController();

  enum class HdcpState : int {
    kUndesired,
    kDesired,
    kRequested,
    kEnabled,
    kRetry,
    kInfeasible,
    kThreadExit
  };

  // Initiates the HDCP enablement process.
  //
  // Sets the internal state to kDesired if it's not currently in a retry state.
  // This signals the display pipeline to include HDCP properties in the next
  // commit.
  void Start();

  // Signals that an HDCP enablement request has been sent to the kernel.
  //
  // This is called after an atomic commit including HDCP properties has been
  // submitted. If HDCP negotiation is in progress (kDesired or kRetry), it
  // transitions the state to kRequested and starts the timer to wait for the
  // hardware to complete authentication.
  void Requested();

  // Retrieves the current HDCP state in a thread-safe manner.
  //
  // Returns the current state of the HDCP state machine.
  HdcpState GetHdcpState() const;

  // Returns true and populates |out_level| when the controller has settled in
  // kEnabled or kInfeasible so a newly attached client can be notified.
  bool GetSettledNotification(std::optional<HdcpContentType>* out_level) const;

  // Terminates the HDCP session.
  //
  // Stops any ongoing HDCP negotiation, resets the state to kUndesired, and
  // notifies the client that HDCP is disabled. This is thread-safe.
  void Terminate();

  // Resets ongoing or completed HDCP state to kUndesired without firing client
  // callbacks (used when detaching a disconnected client session), while
  // preserving terminal kInfeasible hardware/link state until pipeline Deinit.
  void Reset();

  void ExpireRequestTimeoutForTesting();

 private:
  // Verifies the HDCP status on the connector and manages state transitions.
  //
  // This function is typically invoked after a timeout or early check during
  // the HDCP enablement process. It checks if the hardware has successfully
  // enabled content protection for |expected_generation|.
  //
  // State transitions:
  // - Success: Transitions to kEnabled. Notifies client with kType1 (or kType0
  // if retrying).
  // - Failure (when !only_if_enabled):
  //   - If first attempt: Transitions to kRetry and triggers a retry frame
  //   (fallback to Type 0).
  //   - If retry attempt: Transitions to kInfeasible and notifies client of
  //   failure.
  void SetContentProtectionStatus(uint64_t expected_generation,
                                  bool only_if_enabled) EXCLUDES(mutex_);

  // Stop the helper thread
  void StopThread();

  void ThreadFn();

  const DrmDisplayPipeline* pipeline_;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  HdcpState hdcp_state_ GUARDED_BY(mutex_) = HdcpState::kUndesired;
  uint64_t request_generation_ GUARDED_BY(mutex_) = 0;

  bool was_retry_ GUARDED_BY(mutex_) = false;
  std::optional<HdcpContentType> enabled_content_type_ GUARDED_BY(mutex_) =
      std::nullopt;

  // Only accessed from helper thread.
  const HdcpConCallbacks cbks_;
  decltype(std::chrono::system_clock::now()) sleep_until_ GUARDED_BY(mutex_){};
  const std::chrono::milliseconds timeout_;
};

}  // namespace android::drm_hwcomposer