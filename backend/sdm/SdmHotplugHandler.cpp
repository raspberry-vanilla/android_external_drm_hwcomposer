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

#define LOG_TAG "drmhwc"  // NOLINT(cppcoreguidelines-macro-usage)

#include "backend/sdm/SdmHotplugHandler.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include "utils/log.h"

namespace android::drm_hwcomposer {

void SdmHotplugHandler::SetHotplugHandler(HotplugHandler handler) {
  {
    std::scoped_lock lock(mutex_);
    handler_ = std::move(handler);
    if (!handler_) {
      if (!pending_events_.empty()) {
        ALOGW(
            "SdmHotplugHandler: Clearing %zu pending hotplug events on null "
            "handler registration",
            pending_events_.size());
        pending_events_.clear();
      }
      return;
    }
    if (is_dispatching_) {
      ALOGW(
          "SdmHotplugHandler: SetHotplugHandler called while dispatch is "
          "already in progress");
      return;
    }
    is_dispatching_ = true;
  }
  DispatchLoop();
}

void SdmHotplugHandler::HandleHotplugEvent(uint32_t connector_id,
                                           bool connected) {
  {
    std::scoped_lock lock(mutex_);
    pending_events_.push_back(HotplugEvent{
        .connector_id = connector_id,
        .connected = connected,
    });
    if (!handler_) {
      ALOGI(
          "SdmHotplugHandler: Handler not set yet, queued hotplug event for "
          "connector %u (connected=%d)",
          connector_id, connected);
      return;
    }
    if (is_dispatching_) {
      ALOGI(
          "SdmHotplugHandler: Dispatch in progress, queued hotplug event for "
          "connector %u (connected=%d)",
          connector_id, connected);
      return;
    }
    is_dispatching_ = true;
  }
  DispatchLoop();
}

void SdmHotplugHandler::DispatchLoop() {
  while (true) {
    HotplugEvent next_event{};
    HotplugHandler current_handler;
    {
      std::scoped_lock lock(mutex_);
      if (pending_events_.empty() || !handler_) {
        is_dispatching_ = false;
        return;
      }
      next_event = pending_events_.front();
      pending_events_.erase(pending_events_.begin());
      current_handler = handler_;
    }

    if (current_handler) {
      if (!next_event.connected) {
        // Delay the hotplug disconnect for 100ms. This makes multi-display
        // hotplug more reliable, by avoiding suspected issues in the Android
        // Framework resulting in the disconnect event being dropped. Tested
        // over multiple plug/unplug iterations.
        constexpr int kDisconnectDelayMs = 100;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDisconnectDelayMs));
      }
      current_handler(next_event.connector_id, next_event.connected);
    }
  }
}

}  // namespace android::drm_hwcomposer
