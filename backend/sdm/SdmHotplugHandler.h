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

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace android::drm_hwcomposer {

// SdmHotplugHandler manages queuing and dispatching hotplug events from SDM
// in strict FIFO order.
class SdmHotplugHandler {
 public:
  using HotplugHandler = std::function<void(uint32_t /*connector_id*/,
                                            bool /*connected*/)>;

  struct HotplugEvent {
    uint32_t connector_id{0};
    bool connected{false};
  };

  SdmHotplugHandler() = default;
  ~SdmHotplugHandler() = default;

  // Set (or clear if handler is null) the hotplug callback. Drains any pending
  // events.
  void SetHotplugHandler(HotplugHandler handler);

  // Enqueue a hotplug event for the given DRM connector ID and dispatch in FIFO
  // order. If another thread is already dispatching, this will queue the
  // connector and return without dispatching, leaving the active dispatching
  // thread to process it.
  void HandleHotplugEvent(uint32_t connector_id, bool connected);

 private:
  void DispatchLoop();

  mutable std::mutex mutex_;
  HotplugHandler handler_{nullptr};
  std::vector<HotplugEvent> pending_events_;
  bool is_dispatching_{false};
};

}  // namespace android::drm_hwcomposer
