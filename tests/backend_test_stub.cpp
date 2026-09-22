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

#include <memory>
#include <string>

#include "backend/Backend.h"
#include "backend/BackendManager.h"

namespace android::drm_hwcomposer {

BackendManager &BackendManager::GetInstance() {
  static BackendManager backend_manager;
  return backend_manager;
}

void BackendManager::Register(
    const std::string & /*name*/,
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    BackendRegistration /*registration*/) {
}

void BackendManager::InitializeBackends() {
  initialized_ = true;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::unique_ptr<Backend> BackendManager::CreateBackendForDevice(
    DrmDevice & /*drm*/) {
  return nullptr;
}

}   // namespace android::drm_hwcomposer
