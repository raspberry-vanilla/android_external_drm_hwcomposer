/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <cutils/native_handle.h>
#include <sync/sync.h>

#include <cstdarg>
#include <cstdint>
#if defined(ANDROID) && !defined(__BIONIC__)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif
#include <memory>
#include <optional>

#include <core/sdm_types.h>
#include <debug_callback_intf.h>

#include "backend/sdm/SdmDebugCallback.h"
#include "backend/sdm/SnapAllocHandle.h"
#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "bufferinfo/BufferInfoMapperMetadata.h"
#include "bufferinfo/GrallocBufferHandle.h"

struct sync_file_info;

extern "C" {

int sync_wait(int /*fd*/, int /*timeout*/) {
  return 0;
}

struct sync_file_info* sync_file_info(int32_t /*fd*/) {
  return nullptr;
}

void sync_file_info_free(struct sync_file_info* /*info*/) {
}

#if defined(ANDROID) && !defined(__BIONIC__)
// Override libbase's weak host property stubs so ScopedTestProperty can set
// and clear read-only ("ro.*") properties across multiple unit tests.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,concurrency-mt-unsafe)
int __system_property_set(const char* key, const char* value) {
  if (key == nullptr || *key == '\0') {
    return -1;
  }
  if (value == nullptr || *value == '\0') {
    return unsetenv(key);
  }
  return setenv(key, value, 1);
}

int __system_property_get(const char* key, char* value) {
  constexpr size_t kPropValueMax = 92;
  if (key == nullptr || value == nullptr) {
    return 0;
  }
  const char* prop = std::getenv(key);
  if (prop == nullptr) {
    *value = '\0';
    return 0;
  }
  std::snprintf(value, kPropValueMax, "%s", prop);
  return static_cast<int>(std::strlen(value));
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,concurrency-mt-unsafe)
#endif

}  // extern "C"

namespace android::drm_hwcomposer {

std::optional<BufferInfo> BufferInfoMapperMetadata::GetBoInfo(
    buffer_handle_t /*handle*/) {
  return std::nullopt;
}

std::unique_ptr<BufferInfoGetter> BufferInfoMapperMetadata::CreateInstance() {
  return nullptr;
}

// Stub for GrallocBufferHandle::Create
std::shared_ptr<GrallocBufferHandle> GrallocBufferHandle::Create(
    buffer_handle_t handle) {
  auto gralloc_handle = std::shared_ptr<GrallocBufferHandle>(
      new GrallocBufferHandle(handle));
  return gralloc_handle;
}

// Stub for GrallocBufferHandle::~GrallocBufferHandle
// Do not release the handle, since it wasn't imported.
GrallocBufferHandle::~GrallocBufferHandle() = default;

std::shared_ptr<SnapAllocHandle> SnapAllocHandle::Create(
    buffer_handle_t handle) {
  return std::shared_ptr<SnapAllocHandle>(new SnapAllocHandle(handle, nullptr));
}

bool SnapAllocHandle::Init(sdm::DebugCallbackIntf* /*debug_callback*/) {
  return true;
}

SnapAllocHandle::SnapAllocHandle(
    buffer_handle_t buffer_handle,
    vendor::qti::hardware::display::snapalloc::SnapHandle* snap_handle)
    : GrallocBufferHandle(buffer_handle), snap_handle_(snap_handle) {
}

vendor::qti::hardware::display::snapalloc::SnapHandle*
SnapAllocHandle::GetSnapHandle() const {
  return snap_handle_;
}

SnapAllocHandle::~SnapAllocHandle() = default;

void SdmDebugCallback::Log(sdm::DebugLogType /*type*/, const char* /*log_tag*/,
                           const char* /*fmt*/, std::va_list& /*args*/) {
}

int SdmDebugCallback::GetProperty(const char* /*property_name*/,
                                  int* /*value*/) {
  return sdm::kErrorNotSupported;
}

int SdmDebugCallback::GetProperty(const char* /*property_name*/,
                                  char* /*value*/) {
  return sdm::kErrorNotSupported;
}

void SdmDebugCallback::BeginTrace(const char* /*class_name*/,
                                  const char* /*function_name*/,
                                  const char* /*custom_string*/) {
}

void SdmDebugCallback::EndTrace() {
}

void SdmDebugCallback::ATrace(const char* /*custom_string*/, int /*bit*/) {
}

}  // namespace android::drm_hwcomposer
