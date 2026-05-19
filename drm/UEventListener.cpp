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

#include <sys/types.h>
#include <unistd.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "utils/UEvent.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

UEventListener::UEventListener(std::function<void()> hotplug_handler)
    : hotplug_handler_(std::move(hotplug_handler)) {
}

UEventListener::~UEventListener() {
  exit_ = true;
  uevent_->Stop();
  thread_.join();
}

auto UEventListener::CreateInstance(std::function<void()> hotplug_handler)
    -> std::shared_ptr<UEventListener> {
  if (!hotplug_handler) {
    ALOGE("Invalid hotplug handler");
    return {};
  }

  auto uel = std::shared_ptr<UEventListener>(
      new UEventListener(std::move(hotplug_handler)));

  uel->uevent_ = UEvent::CreateInstance();
  if (!uel->uevent_) {
    ALOGE("Failed to create UEvent");
    return {};
  }

  uel->thread_ = std::thread(&UEventListener::ThreadFn, uel.get());
  return uel;
}

void UEventListener::ThreadFn() {
  while (!exit_) {
    auto uevent_str = uevent_->ReadNext();
    if (exit_) {
      break;
    }
    if (!uevent_str) {
      continue;
    }

    auto drm_event = uevent_str->find("DEVTYPE=drm_minor") != std::string::npos;
    auto hotplug_event = uevent_str->find("HOTPLUG=1") != std::string::npos;

    if (drm_event && hotplug_event) {
      constexpr useconds_t kDelayAfterUeventUs = 200000;
      /* We need some delay to ensure DrmConnector::UpdateModes() will query
       * correct modes list, otherwise at least RPI4 board may report 0 modes */
      usleep(kDelayAfterUeventUs);
      if (exit_) {
        break;
      }
      hotplug_handler_();
    }
  }

  ALOGI("UEvent thread exit");
}

}  // namespace android::drm_hwcomposer
