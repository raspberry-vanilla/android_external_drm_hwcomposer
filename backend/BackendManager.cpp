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
#include <vector>

#include "bufferinfo/BufferInfoGetter.h"
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

BackendManager::Backend::Backend(const std::string &name) : name_(name) {
  BackendManager::GetInstance().RegisterBackend(name, this);
}

BackendManager::Backend::~Backend() {
  BackendManager::GetInstance().UnregisterBackend(name_);
}

BackendManager &BackendManager::GetInstance() {
  static BackendManager backend_manager;

  return backend_manager;
}

void BackendManager::RegisterBackend(const std::string &name,
                                     Backend *backend) {
  if (available_backends_.count(name) != 0) {
    ALOGE("Backend %s already registered.", name.c_str());
    return;
  }
  available_backends_[name] = backend;
}

void BackendManager::UnregisterBackend(const std::string &name) {
  available_backends_.erase(name);
}

void BackendManager::InitializeBackends() {
  for (auto it = available_backends_.begin();
       it != available_backends_.end();) {
    bool success = it->second->Init();
    if (!success) {
      ALOGE("Failed to initialize backend %s", it->first.c_str());
      it = available_backends_.erase(it);
    } else {
      ++it;
    }
  }
}

std::unique_ptr<DrmDisplayPipeline> BackendManager::CreatePipelineForConnector(
    DrmConnector &connector) {
  auto driver_name(connector.GetDev().GetName());
  std::string backend_name = Properties::GetBackendOverride();
  if (backend_name.empty()) {
    backend_name = driver_name;
  }

  auto *backend = GetBackendByName(backend_name);
  if (backend == nullptr) {
    ALOGE("Failed to find backend '%s' for '%s' and driver '%s'",
          backend_name.c_str(), connector.GetName().c_str(),
          driver_name.c_str());
    return nullptr;
  }
  ALOGI("Found Backend '%s' for '%s' and driver '%s'", backend_name.c_str(),
        connector.GetName().c_str(), driver_name.c_str());

  return backend->CreatePipeline(connector);
}

bool BackendManager::IsDozeSupported(const std::string &driver_name) {
  std::string backend_name = Properties::GetBackendOverride();
  if (backend_name.empty()) {
    backend_name = driver_name;
  }

  auto *backend = GetBackendByName(backend_name);
  if (backend != nullptr) {
    return backend->SupportsDoze();
  }
  return false;
}

bool BackendManager::IsDozeSuspendSupported(const std::string &driver_name) {
  std::string backend_name = Properties::GetBackendOverride();
  if (backend_name.empty()) {
    backend_name = driver_name;
  }

  auto *backend = GetBackendByName(backend_name);
  if (backend != nullptr) {
    return backend->SupportsDozeSuspend();
  }
  return false;
}

bool BackendManager::IsSuspendSupported(const std::string &driver_name) {
  std::string backend_name = Properties::GetBackendOverride();
  if (backend_name.empty()) {
    backend_name = driver_name;
  }

  auto *backend = GetBackendByName(backend_name);
  if (backend != nullptr) {
    return backend->SupportsSuspend();
  }
  return false;
}

std::unique_ptr<BufferInfoGetter> BackendManager::CreateBufferInfoGetter() {
  // If backend override is not specified, the generic backend will be used.
  std::string backend_name = Properties::GetBackendOverride();
  auto *backend = GetBackendByName(backend_name);
  if (backend == nullptr) {
    ALOGE("Failed to find backend");
    return nullptr;
  }
  return backend->CreateBufferInfoGetter();
}

std::unique_ptr<AtomicCommitSink> BackendManager::CreateAtomicCommitSink(
    const std::string &driver_name) {
  // If backend override is not specified, the generic backend will be used.
  std::string backend_name = Properties::GetBackendOverride();
  if (backend_name.empty()) {
    backend_name = driver_name;
  }

  auto *backend = GetBackendByName(backend_name);
  if (backend == nullptr) {
    ALOGE("Failed to find backend");
    return nullptr;
  }
  return backend->CreateAtomicCommitSink();
}

BackendManager::Backend *BackendManager::GetBackendByName(std::string &name) {
  if (available_backends_.empty()) {
    ALOGE("No backends are specified");
    return nullptr;
  }

  auto it = available_backends_.find(name);
  if (it == available_backends_.end()) {
    auto it = std::find(kClientDevices.begin(), kClientDevices.end(), name);
    name = it == kClientDevices.end() ? "generic" : "client";
  }

  return available_backends_[name];
}

}  // namespace android::drm_hwcomposer
