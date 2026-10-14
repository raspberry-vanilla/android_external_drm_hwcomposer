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

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "bufferinfo/BufferInfoGetter.h"
#include "bufferinfo/BufferInfoMapperMetadata.h"
#include "bufferinfo/GrallocBufferHandle.h"
#include "drm/UEventListener.h"

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

}  // extern "C"

namespace android::drm_hwcomposer {

class UEvent {};

UEventListener::UEventListener(std::function<void()> hotplug_handler)
    : hotplug_handler_(std::move(hotplug_handler)) {
}

UEventListener::~UEventListener() = default;

auto UEventListener::CreateInstance(std::function<void()> hotplug_handler)
    -> std::shared_ptr<UEventListener> {
  if (!hotplug_handler) {
    return {};
  }
  return std::shared_ptr<UEventListener>(
      new UEventListener(std::move(hotplug_handler)));
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

}  // namespace android::drm_hwcomposer
