/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "UEventListener.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "utils/UEvent.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

UEventListener::UEventListener(HotplugHandler hotplug_handler)
    : hotplug_handler_(std::move(hotplug_handler)) {
}

UEventListener::~UEventListener() {
  exit_ = true;
  if (uevent_) {
    uevent_->Stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

auto UEventListener::CreateInstance(HotplugHandler hotplug_handler)
    -> std::shared_ptr<UEventListener> {
  return CreateInstance(std::move(hotplug_handler), nullptr);
}

auto UEventListener::CreateInstance(HotplugHandler hotplug_handler,
                                    std::unique_ptr<UEvent> uevent)
    -> std::shared_ptr<UEventListener> {
  if (!hotplug_handler) {
    ALOGE("Invalid hotplug handler");
    return {};
  }

  auto uel = std::shared_ptr<UEventListener>(
      new UEventListener(std::move(hotplug_handler)));

  uel->uevent_ = uevent ? std::move(uevent) : UEvent::CreateInstance();
  if (!uel->uevent_) {
    ALOGE("Failed to create UEvent");
    return {};
  }

  uel->thread_ = std::thread(&UEventListener::ThreadFn, uel.get());
  return uel;
}

void UEventListener::ThreadFn() {
  constexpr int kSettleDelayMs = 200;
  auto has_field = [](std::string_view event_str, std::string_view field) {
    size_t pos = 0;
    while (pos < event_str.size()) {
      size_t end = event_str.find('\n', pos);
      if (end == std::string_view::npos) {
        end = event_str.size();
      }
      if (event_str.substr(pos, end - pos) == field) {
        return true;
      }
      pos = end + 1;
    }
    return false;
  };
  auto is_drm_hotplug = [&has_field](const std::optional<std::string> &event) {
    if (!event) {
      return false;
    }
    const bool is_drm_minor = has_field(*event, "DEVTYPE=drm_minor");
    const bool is_drm_connector = has_field(*event, "DEVTYPE=drm_connector");
    const bool is_hotplug = has_field(*event, "HOTPLUG=1");
    if (is_drm_minor && is_hotplug) {
      return true;
    }
    if (is_drm_connector) {
      const bool is_add = has_field(*event, "ACTION=add");
      const bool is_remove = has_field(*event, "ACTION=remove");
      return is_hotplug || is_add || is_remove;
    }
    return false;
  };

  while (!exit_) {
    auto uevent_str = uevent_->ReadNext();
    if (exit_) {
      break;
    }
    if (!is_drm_hotplug(uevent_str)) {
      continue;
    }

    while (!exit_) {
      // Drain any back-to-back uevents already queued in the socket buffer.
      while (!exit_) {
        auto queued = uevent_->ReadNextNonBlocking();
        if (!queued) {
          break;
        }
      }
      if (exit_) {
        break;
      }

      const bool needs_settle_retry = hotplug_handler_();

      // Wait up to 200ms from the end of this hotplug pass for either a
      // follow-up hotplug uevent (e.g. a rapid unplug/replug or multi-stream
      // MST burst) or a quiet settle timeout (ensuring boards like RPI4 that
      // report 0 modes or unknown connection status on the initial pulse are
      // re-probed after 200ms). Use a fixed steady_clock deadline so unrelated
      // non-DRM uevents do not reset the 200ms window.
      const auto settle_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(kSettleDelayMs);
      bool saw_followup_hotplug = false;
      while (!exit_) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= settle_deadline) {
          break;
        }
        const auto remaining_ms = std::chrono::duration_cast<
                                      std::chrono::milliseconds>(
                                      settle_deadline - now)
                                      .count();
        const int timeout_ms = std::max(1, static_cast<int>(remaining_ms));
        auto followup = uevent_->ReadNextWithTimeout(timeout_ms);
        if (exit_ || !followup) {
          break;
        }
        if (is_drm_hotplug(followup)) {
          saw_followup_hotplug = true;
          break;
        }
      }
      if (exit_) {
        break;
      }
      if (!saw_followup_hotplug) {
        if (needs_settle_retry) {
          hotplug_handler_();
        }
        break;
      }
    }
  }

  ALOGI("UEvent thread exit");
}

}  // namespace android::drm_hwcomposer
