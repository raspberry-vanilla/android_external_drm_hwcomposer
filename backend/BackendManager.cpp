/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "BackendManager.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "backend/Backend.h"
#include "drm/DrmConnector.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

namespace {
// List of devices that should default to client composition.
// NOLINTNEXTLINE(cert-err58-cpp)
const std::vector<std::string> kClientDevices = {
    "kirin",
    "mediatek-drm",
    "pl111",
};
}  // namespace

BackendManager &BackendManager::GetInstance() {
  static BackendManager backend_manager;
  return backend_manager;
}

void BackendManager::Register(const std::string &name,
                              BackendRegistration registration) {
  if (backends_.count(name) != 0) {
    ALOGE("Backend creator for %s already registered.", name.c_str());
    return;
  }
  backends_[name] = std::move(registration);
}

void BackendManager::InitializeBackends() {
  if (initialized_) {
    return;
  }
  initialized_ = true;
  for (auto it = backends_.begin(); it != backends_.end();) {
    if (it->second.init && !it->second.init()) {
      ALOGE("Failed to initialize backend %s", it->first.c_str());
      it = backends_.erase(it);
    } else {
      ++it;
    }
  }
}

std::unique_ptr<Backend> BackendManager::CreateBackendForDevice(
    DrmDevice &drm) {
  if (backends_.empty()) {
    ALOGE("No backends are registered");
    return nullptr;
  }

  std::string name = Properties::GetBackendOverride();
  if (name.empty()) {
    name = drm.GetName();
  }

  auto it = backends_.find(name);
  if (it == backends_.end()) {
    auto client_it = std::find(kClientDevices.begin(), kClientDevices.end(),
                               name);
    name = client_it == kClientDevices.end() ? "generic" : "client";
  }

  ALOGI("Creating backend '%s' for device '%s'", name.c_str(),
        drm.GetName().c_str());
  return backends_[name].creator(drm);
}

}  // namespace android::drm_hwcomposer
