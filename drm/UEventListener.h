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
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#include "utils/UEvent.h"

namespace android::drm_hwcomposer {

class UEventListener {
 public:
  using HotplugHandler = std::function<bool()>;

  ~UEventListener();

  static auto CreateInstance(HotplugHandler hotplug_handler)
      -> std::shared_ptr<UEventListener>;
  static auto CreateInstance(HotplugHandler hotplug_handler,
                             std::unique_ptr<UEvent> uevent)
      -> std::shared_ptr<UEventListener>;

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
  explicit UEventListener(HotplugHandler hotplug_handler);

  void ThreadFn();

  std::thread thread_;
  std::atomic<bool> exit_ = false;
  std::unique_ptr<UEvent> uevent_;

  HotplugHandler hotplug_handler_;
};

}  // namespace android::drm_hwcomposer
