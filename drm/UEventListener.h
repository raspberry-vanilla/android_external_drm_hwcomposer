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

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>

#include "utils/UEvent.h"

namespace android::drm_hwcomposer {

struct UEventHotplugEvent {
  std::set<uint32_t> connector_ids;
  bool refresh_all = false;

  void Merge(const UEventHotplugEvent &other) {
    connector_ids.insert(other.connector_ids.begin(),
                         other.connector_ids.end());
    refresh_all = refresh_all || other.refresh_all;
  }
};

class UEventListener {
 public:
  using HotplugEvent = UEventHotplugEvent;

  // Returns true if at least one connector remained unsettled (e.g. connected
  // with 0 modes, unknown connection, or transient vanish/probe failure) so a
  // quiet 200ms trailing settle re-probe is required; false when settled.
  using HotplugHandler = std::function<bool()>;
  using ScopedSettleHotplugHandler = std::function<bool(const HotplugEvent &)>;

  ~UEventListener();

  static auto CreateInstance(HotplugHandler hotplug_handler)
      -> std::shared_ptr<UEventListener>;
  static auto CreateInstance(HotplugHandler hotplug_handler,
                             std::unique_ptr<UEvent> uevent)
      -> std::shared_ptr<UEventListener>;
  static auto CreateWithScopedSettleHandler(
      ScopedSettleHotplugHandler hotplug_handler)
      -> std::shared_ptr<UEventListener>;
  static auto CreateWithScopedSettleHandler(
      ScopedSettleHotplugHandler hotplug_handler,
      std::unique_ptr<UEvent> uevent) -> std::shared_ptr<UEventListener>;

  template <typename F, typename = std::enable_if_t<
                            std::is_void_v<std::invoke_result_t<F>>>>
  static auto CreateInstance(F &&hotplug_handler,
                             std::unique_ptr<UEvent> uevent = nullptr)
      -> std::shared_ptr<UEventListener> {
    return CreateInstance(HotplugHandler(
                              [handler = std::forward<F>(hotplug_handler)]() {
                                handler();
                                return false;
                              }),
                          std::move(uevent));
  }

 private:
  explicit UEventListener(ScopedSettleHotplugHandler hotplug_handler);

  void ThreadFn();

  std::thread thread_;
  std::atomic<bool> exit_ = false;
  std::unique_ptr<UEvent> uevent_;

  ScopedSettleHotplugHandler hotplug_handler_;
};

}  // namespace android::drm_hwcomposer
