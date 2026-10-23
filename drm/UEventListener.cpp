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
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "utils/UEvent.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

namespace {

auto HasUEventField(std::string_view event_str, std::string_view field)
    -> bool {
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
}

auto ParseDrmHotplugUEvent(const std::optional<std::string> &event)
    -> std::optional<UEventListener::HotplugEvent> {
  if (!event) {
    return std::nullopt;
  }
  const bool is_drm_minor = HasUEventField(*event, "DEVTYPE=drm_minor");
  const bool is_drm_connector = HasUEventField(*event, "DEVTYPE=drm_connector");
  const bool is_hotplug = HasUEventField(*event, "HOTPLUG=1");
  bool relevant = false;
  if (is_drm_minor && is_hotplug) {
    relevant = true;
  } else if (is_drm_connector) {
    const bool is_add = HasUEventField(*event, "ACTION=add");
    const bool is_remove = HasUEventField(*event, "ACTION=remove");
    relevant = is_hotplug || is_add || is_remove;
  }
  if (!relevant) {
    return std::nullopt;
  }

  UEventListener::HotplugEvent parsed{};
  if (is_drm_minor && is_hotplug && !is_drm_connector) {
    constexpr std::string_view kConnectorPrefix = "CONNECTOR=";
    std::string_view remaining(*event);
    bool found_connector_id = false;
    while (!remaining.empty()) {
      const size_t newline_pos = remaining.find('\n');
      const std::string_view line = (newline_pos == std::string_view::npos)
                                        ? remaining
                                        : remaining.substr(0, newline_pos);
      if (line.substr(0, kConnectorPrefix.size()) == kConnectorPrefix) {
        const std::string_view id_str = line.substr(kConnectorPrefix.size());
        uint32_t conn_id = 0;
        bool valid_id = !id_str.empty();
        for (const char c : id_str) {
          if (c < '0' || c > '9') {
            valid_id = false;
            break;
          }
          constexpr uint32_t kDecimalBase = 10;
          const auto digit = static_cast<uint32_t>(c - '0');
          if (conn_id >
              (std::numeric_limits<uint32_t>::max() - digit) / kDecimalBase) {
            valid_id = false;
            break;
          }
          conn_id = (conn_id * kDecimalBase) + digit;
        }
        if (valid_id && conn_id > 0) {
          parsed.connector_ids.insert(conn_id);
          found_connector_id = true;
        }
      }
      if (newline_pos == std::string_view::npos) {
        break;
      }
      remaining.remove_prefix(newline_pos + 1);
    }
    if (!found_connector_id) {
      parsed.refresh_all = true;
    }
  } else {
    parsed.refresh_all = true;
  }

  return parsed;
}

}  // namespace

UEventListener::UEventListener(ScopedSettleHotplugHandler hotplug_handler)
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
  return CreateWithScopedSettleHandler([handler = std::move(hotplug_handler)](
                                           const HotplugEvent & /*event*/)
                                           -> bool { return handler(); },
                                       std::move(uevent));
}

auto UEventListener::CreateWithScopedSettleHandler(
    ScopedSettleHotplugHandler hotplug_handler)
    -> std::shared_ptr<UEventListener> {
  return CreateWithScopedSettleHandler(std::move(hotplug_handler), nullptr);
}

auto UEventListener::CreateWithScopedSettleHandler(
    ScopedSettleHotplugHandler hotplug_handler, std::unique_ptr<UEvent> uevent)
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

  while (!exit_) {
    auto uevent_str = uevent_->ReadNext();
    if (exit_) {
      break;
    }
    auto first_parsed = ParseDrmHotplugUEvent(uevent_str);
    if (!first_parsed) {
      continue;
    }

    HotplugEvent pending_event = std::move(*first_parsed);
    while (!exit_) {
      // Drain any back-to-back uevents already queued in the socket buffer.
      while (!exit_) {
        auto queued = uevent_->ReadNextNonBlocking();
        if (!queued) {
          break;
        }
        if (auto queued_parsed = ParseDrmHotplugUEvent(queued)) {
          pending_event.Merge(*queued_parsed);
        }
      }
      if (exit_) {
        break;
      }

      HotplugEvent dispatched_event = std::move(pending_event);
      pending_event = HotplugEvent{};
      const bool needs_settle_reprobe = hotplug_handler_(dispatched_event);

      // Wait up to 200ms from the end of this hotplug pass for either a
      // follow-up hotplug uevent (e.g. a rapid unplug/replug or multi-stream
      // MST burst) or a quiet settle timeout (ensuring boards like RPI4 that
      // report 0 modes or unknown connection status on the initial pulse are
      // re-probed after 200ms while skipping duplicate sweeps when settled).
      // Use a fixed steady_clock deadline so unrelated non-DRM uevents do not
      // reset the 200ms window.
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
        if (auto followup_parsed = ParseDrmHotplugUEvent(followup)) {
          if (needs_settle_reprobe) {
            pending_event.Merge(dispatched_event);
          }
          pending_event.Merge(*followup_parsed);
          saw_followup_hotplug = true;
          break;
        }
      }
      if (exit_) {
        break;
      }
      if (!saw_followup_hotplug) {
        if (needs_settle_reprobe) {
          hotplug_handler_(dispatched_event);
        }
        break;
      }
    }
  }

  ALOGI("UEvent thread exit");
}

}  // namespace android::drm_hwcomposer
