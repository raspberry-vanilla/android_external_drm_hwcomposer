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

#include "backend/BackendManager.h"

#include "utils/log.h"

namespace android::drm_hwcomposer {
BackendManager &BackendManager::GetInstance() {
  static BackendManager backend_manager;
  return backend_manager;
}

void BackendManager::RegisterBackend(const std::string &/*name*/, Backend */*backend*/){
}

void BackendManager::UnregisterBackend(const std::string &/*name*/){
}

void BackendManager::InitializeBackends(){
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::unique_ptr<DrmDisplayPipeline> BackendManager::CreatePipelineForConnector(
    DrmConnector &/*connector*/){
        return nullptr;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::unique_ptr<BufferInfoGetter> BackendManager::CreateBufferInfoGetter(){
        return nullptr;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool BackendManager::IsDozeSupported(const std::string &/*driver_name*/) {
    return false;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool BackendManager::IsDozeSuspendSupported(const std::string &/*driver_name*/) {
    return false;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool BackendManager::IsSuspendSupported(const std::string &/*driver_name*/) {
    return false;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::unique_ptr<AtomicCommitSink> BackendManager::CreateAtomicCommitSink(
      const std::string &/*driver_name*/){
        return nullptr;
}
}   // namespace android::drm_hwcomposer
